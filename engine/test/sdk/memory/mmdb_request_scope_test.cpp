/**
 * @file mmdb_request_scope_test.cpp
 * @brief Task 9：请求级内存上下文作用域集成测试
 *
 * 覆盖以下能力：
 * 1. mmdb_request_begin 创建请求上下文并切换为当前上下文
 * 2. mmdb_request_end 恢复旧上下文并销毁请求上下文
 * 3. 请求上下文中分配的内存随 end 自动释放
 * 4. 嵌套请求作用域（父子层级）
 * 5. 无效参数返回错误码
 * 6. end 对非活跃作用域安全（no-op）
 * 7. 资源析构器在 end 时触发
 */

// 注意：必须先包含 gtest，再包含 memctx.h。
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "sdk/mmdb.h"
#include "sdk/impl/mmdb_internal.h"  /* 暴露 struct mmdb_s 真实结构 */
#include "sdk/impl/memctx.h"           /* MemoryContextData 字段访问 */
}

namespace {

constexpr const char* kDbPath = "test_request_scope.db";

/**
 * @brief 测试夹具：默认创建一个内存数据库用于请求作用域测试
 */
class MmdbRequestScopeTest : public ::testing::Test {
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

/* ========================================================================
 * 测试 1：begin 创建请求上下文并切换为当前上下文
 * ======================================================================== */
TEST_F(MmdbRequestScopeTest, BeginCreatesContext) {
    db = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db, nullptr);

    mmdb_request_scope_t scope;

    int rc = mmdb_request_begin(db, "test-request", &scope);
    ASSERT_EQ(rc, MMDB_OK);
    EXPECT_TRUE(scope.active);
    EXPECT_NE(scope.context, nullptr);

    /* 请求上下文应为 connection_context 的子节点 */
    ASSERT_NE(scope.context->parent, nullptr);
    EXPECT_EQ(scope.context->parent, db->connection_context);

    /* 当前上下文应为请求上下文 */
    EXPECT_EQ(MemoryContextCurrent(), scope.context);

    /* previous 不应等于请求上下文本身 */
    EXPECT_NE(scope.previous, scope.context);

    mmdb_request_end(&scope);
}

/* ========================================================================
 * 测试 2：end 恢复旧上下文并销毁请求上下文
 * ======================================================================== */
TEST_F(MmdbRequestScopeTest, EndRestoresPreviousContext) {
    db = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db, nullptr);

    /* 记录 begin 之前的当前上下文 */
    MemoryContext prev = MemoryContextCurrent();

    mmdb_request_scope_t scope;
    int rc = mmdb_request_begin(db, "test-end", &scope);
    ASSERT_EQ(rc, MMDB_OK);
    EXPECT_EQ(MemoryContextCurrent(), scope.context);

    /* end 应恢复到之前的上下文 */
    mmdb_request_end(&scope);

    EXPECT_EQ(MemoryContextCurrent(), prev);
    EXPECT_FALSE(scope.active);
    EXPECT_EQ(scope.context, nullptr);
}

/* ========================================================================
 * 测试 3：请求上下文中分配的内存在 end 时自动释放
 * ======================================================================== */
TEST_F(MmdbRequestScopeTest, AllocationsFreedOnEnd) {
    db = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db, nullptr);

    mmdb_request_scope_t scope;
    int rc = mmdb_request_begin(db, "alloc-test", &scope);
    ASSERT_EQ(rc, MMDB_OK);

    /* 在请求上下文中分配内存 */
    void* ptr = palloc(scope.context, 1024);
    ASSERT_NE(ptr, nullptr);
    EXPECT_GT(scope.context->current_bytes, 0u);

    /* end 后请求上下文应被标记删除（所有分配由 LIFO 析构 + 块释放回收） */
    MemoryContext ctx = scope.context;
    ASSERT_NE(ctx, nullptr);

    mmdb_request_end(&scope);

    EXPECT_TRUE(ctx->is_deleted);
}

/* ========================================================================
 * 测试 4：嵌套请求作用域（父子层级）
 *
 * 注意：所有请求上下文都以 db->connection_context 为父节点，
 * 并不直接以另一个请求上下文为父（保持平坦层级）。
 * 嵌套体现为"current 上下文链"——内层 begin 切换到 inner，
 * 内层 end 恢复 outer。
 * ======================================================================== */
TEST_F(MmdbRequestScopeTest, NestedScopes) {
    db = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db, nullptr);

    /* 外层请求 */
    mmdb_request_scope_t outer;
    int rc = mmdb_request_begin(db, "outer", &outer);
    ASSERT_EQ(rc, MMDB_OK);
    EXPECT_EQ(MemoryContextCurrent(), outer.context);

    /* 内层请求 */
    mmdb_request_scope_t inner;
    rc = mmdb_request_begin(db, "inner", &inner);
    ASSERT_EQ(rc, MMDB_OK);
    EXPECT_EQ(MemoryContextCurrent(), inner.context);

    /* 内外层上下文都以 connection_context 为父（平坦层级） */
    EXPECT_EQ(outer.context->parent, db->connection_context);
    EXPECT_EQ(inner.context->parent, db->connection_context);

    /* 保留指针以便 end 后仍可验证 is_deleted */
    MemoryContext outer_ctx_ptr = outer.context;
    MemoryContext inner_ctx_ptr = inner.context;

    /* 内层结束后恢复到外层上下文 */
    mmdb_request_end(&inner);
    EXPECT_EQ(MemoryContextCurrent(), outer_ctx_ptr);
    EXPECT_TRUE(inner_ctx_ptr->is_deleted);
    /* 外层上下文仍应处于活跃状态 */
    EXPECT_FALSE(outer_ctx_ptr->is_deleted);

    /* 外层结束后恢复到全局 */
    mmdb_request_end(&outer);
    EXPECT_TRUE(outer_ctx_ptr->is_deleted);
}

/* ========================================================================
 * 测试 5：无效参数返回错误码
 * ======================================================================== */
TEST_F(MmdbRequestScopeTest, InvalidArgsReturnError) {
    db = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db, nullptr);

    mmdb_request_scope_t scope;

    /* db 为 NULL */
    EXPECT_EQ(mmdb_request_begin(nullptr, "test", &scope), MMDB_ERR_INVALID);

    /* scope 为 NULL */
    EXPECT_EQ(mmdb_request_begin(db, "test", nullptr), MMDB_ERR_INVALID);
}

/* ========================================================================
 * 测试 6：end 对非活跃作用域安全（no-op）
 * ======================================================================== */
TEST_F(MmdbRequestScopeTest, EndOnInactiveIsNoop) {
    /* 全零初始化的 scope，active == 0，end 应安全返回 */
    mmdb_request_scope_t scope;
    memset(&scope, 0, sizeof(scope));
    EXPECT_NO_FATAL_FAILURE(mmdb_request_end(&scope));

    /* end 本身应幂等（active 字段保持 0） */
    EXPECT_EQ(scope.active, 0);

    /* end 对 NULL 安全 */
    EXPECT_NO_FATAL_FAILURE(mmdb_request_end(nullptr));
}

/* ========================================================================
 * 测试 7：资源析构器在 end 时触发
 * ======================================================================== */
TEST_F(MmdbRequestScopeTest, ResourceDestructorRunsOnEnd) {
    db = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db, nullptr);

    static int destructor_called = 0;
    destructor_called = 0;

    auto destructor = [](void* /*resource*/, void* /*arg*/) {
        ++destructor_called;
    };

    mmdb_request_scope_t scope;
    int rc = mmdb_request_begin(db, "with-resource", &scope);
    ASSERT_EQ(rc, MMDB_OK);

    /* 注册资源到请求上下文 */
    int dummy = 42;
    EXPECT_EQ(mmdb_mem_register_resource(scope.context, &dummy, destructor,
                                          nullptr, "test-res"),
              0);

    /* end 应触发析构 */
    mmdb_request_end(&scope);
    EXPECT_EQ(destructor_called, 1);
}

/* ========================================================================
 * 测试 8：嵌套作用域中内层分配的内存随内层 end 释放
 * ======================================================================== */
TEST_F(MmdbRequestScopeTest, InnerAllocationsFreedOnInnerEnd) {
    db = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db, nullptr);

    mmdb_request_scope_t outer;
    int rc = mmdb_request_begin(db, "outer-alloc", &outer);
    ASSERT_EQ(rc, MMDB_OK);

    mmdb_request_scope_t inner;
    rc = mmdb_request_begin(db, "inner-alloc", &inner);
    ASSERT_EQ(rc, MMDB_OK);

    /* 在内层分配 */
    void* ptr = palloc(inner.context, 256);
    ASSERT_NE(ptr, nullptr);

    MemoryContext inner_ctx_ptr = inner.context;
    MemoryContext outer_ctx_ptr = outer.context;
    EXPECT_GT(inner_ctx_ptr->current_bytes, 0u);

    /* 结束内层：内层内存被释放，但外层不受影响 */
    mmdb_request_end(&inner);
    EXPECT_TRUE(inner_ctx_ptr->is_deleted);
    EXPECT_FALSE(outer_ctx_ptr->is_deleted);
    EXPECT_EQ(MemoryContextCurrent(), outer_ctx_ptr);

    /* 外层仍可继续分配 */
    void* outer_ptr = palloc(outer_ctx_ptr, 128);
    EXPECT_NE(outer_ptr, nullptr);

    mmdb_request_end(&outer);
    EXPECT_TRUE(outer_ctx_ptr->is_deleted);
}
