/**
 * @file mmdb_root_context_test.cpp
 * @brief Task 8：mmdb_t 数据库三层内存上下文集成测试
 *
 * 覆盖以下能力：
 * 1. mmdb_open 自动创建 DatabaseContext / ConnectionContext / CacheContext
 * 2. 三层上下文的父子层级关系正确
 * 3. mmdb_close 触发根上下文 is_deleted 标记
 * 4. path / last_err_msg 由 memory_context 管理
 * 5. 错误信息可被正确写入与读取
 * 6. 重启 mmdb 后旧上下文不影响新上下文
 */

// 注意：必须先包含 gtest，再包含 memctx.h。
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include "sdk/mmdb.h"
#include "sdk/mmdb_error.h"
#include "sdk/impl/mmdb_internal.h"  /* 暴露 struct mmdb_s 真实结构 */
#include "sdk/impl/memctx.h"           /* MemoryContextData 字段访问 */
}

namespace {

constexpr const char* kDbPath = "test_root_context.db";

class MmdbRootContextTest : public ::testing::Test {
   protected:
    mmdb_t* db = nullptr;

    void SetUp() override { std::remove(kDbPath); }

    void TearDown() override {
        if (db) {
            mmdb_close(db);
            db = nullptr;
        }
        std::remove(kDbPath);
    }
};

}  // namespace

// 测试 1：mmdb_open 创建三层内存上下文
TEST_F(MmdbRootContextTest, OpenCreatesContexts) {
    db = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db, nullptr);

    /* 三层上下文均已创建 */
    EXPECT_NE(db->memory_context, nullptr);
    EXPECT_NE(db->connection_context, nullptr);
    EXPECT_NE(db->cache_context, nullptr);
}

// 测试 2：三层上下文具有正确的父子层级
TEST_F(MmdbRootContextTest, ContextHierarchyIsCorrect) {
    db = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db, nullptr);

    /* DatabaseContext 是根（无父） */
    EXPECT_EQ(db->memory_context->parent, nullptr);
    /* ConnectionContext 与 CacheContext 都是 DatabaseContext 的子节点 */
    EXPECT_EQ(db->connection_context->parent, db->memory_context);
    EXPECT_EQ(db->cache_context->parent, db->memory_context);
}

// 测试 3：上下文名称正确
TEST_F(MmdbRootContextTest, ContextNamesAreCorrect) {
    db = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db, nullptr);

    ASSERT_NE(db->memory_context->name, nullptr);
    ASSERT_NE(db->connection_context->name, nullptr);
    ASSERT_NE(db->cache_context->name, nullptr);

    EXPECT_STREQ(db->memory_context->name, "DatabaseContext");
    EXPECT_STREQ(db->connection_context->name, "ConnectionContext");
    EXPECT_STREQ(db->cache_context->name, "CacheContext");
}

// 测试 4：mmdb_close 触发根上下文删除（is_deleted 标志）
TEST_F(MmdbRootContextTest, CloseDeletesRootContext) {
    db = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db, nullptr);

    MemoryContext root = db->memory_context;
    ASSERT_NE(root, nullptr);
    EXPECT_FALSE(root->is_deleted);

    mmdb_close(db);
    db = nullptr;

    /* 根上下文应已被标记为删除 */
    EXPECT_TRUE(root->is_deleted);
}

// 测试 5：path 由 memory_context 管理（分配自 ctx）
TEST_F(MmdbRootContextTest, PathAllocatedFromContext) {
    db = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db, nullptr);

    EXPECT_NE(db->path, nullptr);
    EXPECT_STREQ(db->path, kDbPath);
}

// 测试 6：错误信息通过 memory_context 管理（mmdb_set_error 走 ctx 分配）
TEST_F(MmdbRootContextTest, ErrorMessagesAreTracked) {
    db = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db, nullptr);

    /* 初始 last_err 应为 MMDB_OK */
    EXPECT_EQ(db->last_err, MMDB_OK);

    /* 设置一个错误 */
    mmdb_set_error(db, MMDB_ERR_INVALID, "test invalid arg");
    EXPECT_EQ(db->last_err, MMDB_ERR_INVALID);
    EXPECT_NE(db->last_err_msg, nullptr);
    EXPECT_STREQ(db->last_err_msg, "test invalid arg");

    /* mmdb_last_error_message 应返回相同内容 */
    const char* msg = mmdb_last_error_message(db);
    ASSERT_NE(msg, nullptr);
    EXPECT_STREQ(msg, "test invalid arg");
}

// 测试 7：错误信息覆盖：连续调用 mmdb_set_error 不应崩溃
TEST_F(MmdbRootContextTest, ErrorMessageOverwrite) {
    db = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db, nullptr);

    mmdb_set_error(db, MMDB_ERR_INVALID, "first error message");
    ASSERT_NE(db->last_err_msg, nullptr);
    EXPECT_STREQ(db->last_err_msg, "first error message");

    /* 覆盖：旧块由 memory_context 统一回收，无需手动 free */
    mmdb_set_error(db, MMDB_ERR_IO, "second error message");
    EXPECT_EQ(db->last_err, MMDB_ERR_IO);
    EXPECT_NE(db->last_err_msg, nullptr);
    EXPECT_STREQ(db->last_err_msg, "second error message");

    /* 再次覆盖 */
    mmdb_set_error(db, MMDB_ERR_NOMEM, "third error");
    EXPECT_STREQ(db->last_err_msg, "third error");
}

// 测试 8：超长错误信息被截断到 MMDB_ERR_MSG_MAX-1
TEST_F(MmdbRootContextTest, LongErrorMessageIsTruncated) {
    db = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db, nullptr);

    std::string long_msg(MMDB_ERR_MSG_MAX * 2, 'x');
    mmdb_set_error(db, MMDB_ERR_INVALID, long_msg.c_str());

    ASSERT_NE(db->last_err_msg, nullptr);
    /* 截断后字符串长度应 <= MMDB_ERR_MSG_MAX-1 */
    EXPECT_LE(std::strlen(db->last_err_msg), (size_t)(MMDB_ERR_MSG_MAX - 1));
}

// 测试 9：:memory: 数据库也能正常创建上下文
TEST_F(MmdbRootContextTest, InMemoryDatabaseCreatesContexts) {
    db = mmdb_open(":memory:", nullptr);
    ASSERT_NE(db, nullptr);

    EXPECT_NE(db->memory_context, nullptr);
    EXPECT_NE(db->connection_context, nullptr);
    EXPECT_NE(db->cache_context, nullptr);

    /* path 应为 ":memory:" */
    EXPECT_STREQ(db->path, ":memory:");
}

// 测试 10：mmdb_open 失败路径不残留（NULL path）
TEST(MmdbRootContextOpen, NullPathReturnsNull) {
    mmdb_t* result = mmdb_open(nullptr, nullptr);
    EXPECT_EQ(result, nullptr);
}

// 测试 11：连续 open/close 不污染（每次创建独立三层上下文）
TEST_F(MmdbRootContextTest, ReopenDoesNotPollute) {
    db = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db, nullptr);
    MemoryContext first_root = db->memory_context;
    /* 在第一个 ctx 中写入错误信息（验证后续关闭会被统一回收） */
    mmdb_set_error(db, MMDB_ERR_INVALID, "first session error");
    mmdb_close(db);
    db = nullptr;
    EXPECT_TRUE(first_root->is_deleted);

    /* 重新打开：新建的上下文应是全新可用状态 */
    db = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db, nullptr);
    EXPECT_NE(db->memory_context, nullptr);
    EXPECT_FALSE(db->memory_context->is_deleted);
    /* 新上下文可正常分配（验证非悬挂/已删除状态） */
    EXPECT_NE(mmdb_mem_alloc(db->connection_context, 64), nullptr);
    /* 新会话的 last_err 应回到初始状态 */
    EXPECT_EQ(db->last_err, MMDB_OK);
}