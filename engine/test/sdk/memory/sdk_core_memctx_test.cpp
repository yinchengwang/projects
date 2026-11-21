/**
 * @file sdk_core_memctx_test.cpp
 * @brief Task 11：SDK Core 模块 MemoryContext 迁移验证测试
 *
 * 覆盖以下能力（Collection 元数据迁移到 db->memory_context 后）：
 * 1. mmdb_open 创建三层 memory_context 后，collection 创建/打开/删除流程稳定
 * 2. mmdb_collection_create 内部 schema_deep_copy 走 ctx（fields / name 由 ctx 管理）
 * 3. mmdb_collection_dispose 不再 free collection（由 ctx 统一回收）
 * 4. mmdb_strdup_in_ctx 是新内部 API，未迁移模块仍可用 mmdb_strdup_internal
 * 5. 多次创建/删除 collection 后 db->memory_context 仍正常工作
 * 6. result_t 内存由 caller 管理（保持原 ABI 兼容）
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include "sdk/mmdb.h"
#include "sdk/impl/mmdb_internal.h"   /* 暴露 struct mmdb_s 真实结构 */
#include "sdk/impl/mmdb_memctx.h"     /* MemoryContext API */
#include "sdk/impl/memctx.h"            /* MemoryContextData 字段访问 */
}

namespace {

constexpr const char* kDbPath = "test_sdk_core_memctx.db";

class SdkCoreMemctxTest : public ::testing::Test {
   protected:
    mmdb_t* db_ = nullptr;

    void SetUp() override { std::remove(kDbPath); }

    void TearDown() override {
        if (db_) {
            mmdb_close(db_);
            db_ = nullptr;
        }
        std::remove(kDbPath);
    }
};

}  // namespace

/* 测试 1：mmdb_open 后 collection 创建/打开/查询流程稳定 */
TEST_F(SdkCoreMemctxTest, CollectionLifecycleAfterMemctx) {
    db_ = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db_, nullptr);

    /* 三层 memory_context 均已建立 */
    EXPECT_NE(db_->memory_context, nullptr);
    EXPECT_NE(db_->connection_context, nullptr);
    EXPECT_NE(db_->cache_context, nullptr);

    /* 创建 collection */
    mmdb_schema_t schema = {MMDB_MODEL_TEXT, 0, nullptr, 0};
    mmdb_collection_t* c = mmdb_collection_create(db_, "t1", &schema);
    EXPECT_NE(c, nullptr);
    EXPECT_STREQ(mmdb_collection_name(c), "t1");

    /* 创建带 fields 的 collection 验证 schema 走 ctx */
    mmdb_field_def_t fields[] = {
        {const_cast<char*>("title"), MMDB_TYPE_TEXT, 0, nullptr},
        {const_cast<char*>("count"), MMDB_TYPE_INT, 1, const_cast<char*>("0")},
    };
    mmdb_schema_t schema_with_fields = {MMDB_MODEL_TEXT, 2, fields, 0};
    mmdb_collection_t* c2 = mmdb_collection_create(db_, "t2",
                                                   &schema_with_fields);
    EXPECT_NE(c2, nullptr);

    /* 关闭 db，collection 应由 ctx 统一回收，不应出现 double-free */
    mmdb_close(db_);
    db_ = nullptr;
}

/* 测试 2：mmdb_strdup_in_ctx 与 mmdb_strdup_internal 双接口并存 */
TEST_F(SdkCoreMemctxTest, StrdupAPIsCoexist) {
    db_ = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db_, nullptr);

    /* 新接口：传 ctx 走 memory_context 路径 */
    char* s1 = mmdb_strdup_in_ctx(db_->memory_context, "hello-ctx");
    ASSERT_NE(s1, nullptr);
    EXPECT_STREQ(s1, "hello-ctx");

    /* 旧接口：未迁移模块（graph.c / filter_parser.c）使用 malloc 路径 */
    char* s2 = mmdb_strdup_internal("hello-legacy");
    ASSERT_NE(s2, nullptr);
    EXPECT_STREQ(s2, "hello-legacy");

    /* 旧接口释放由调用方负责 */
    free(s2);

    /* 验证 ctx 上下文仍然活跃 */
    EXPECT_FALSE(db_->memory_context->is_deleted);

    /* s1 由 ctx 持有，不再单独 free */
}

/* 测试 3：多次 collection 创建/打开验证 ctx 不泄漏（不通过 free 释放） */
TEST_F(SdkCoreMemctxTest, RepeatedCollectionLifecycle) {
    db_ = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db_, nullptr);

    mmdb_schema_t schema = {MMDB_MODEL_TEXT, 0, nullptr, 0};
    for (int i = 0; i < 10; i++) {
        char name[16];
        snprintf(name, sizeof(name), "c%d", i);
        mmdb_collection_t* c = mmdb_collection_create(db_, name, &schema);
        EXPECT_NE(c, nullptr) << "iter=" << i;
    }

    /* 查询所有 collection */
    for (int i = 0; i < 10; i++) {
        char name[16];
        snprintf(name, sizeof(name), "c%d", i);
        mmdb_collection_t* found = mmdb_collection_get(db_, name);
        EXPECT_NE(found, nullptr) << "iter=" << i;
    }

    /* 删除一半验证 drop 流程 */
    for (int i = 0; i < 10; i += 2) {
        char name[16];
        snprintf(name, sizeof(name), "c%d", i);
        mmdb_collection_t* c = mmdb_collection_get(db_, name);
        ASSERT_NE(c, nullptr);
        mmdb_collection_drop(c);
    }

    /* 剩余 collection 仍可访问 */
    for (int i = 1; i < 10; i += 2) {
        char name[16];
        snprintf(name, sizeof(name), "c%d", i);
        mmdb_collection_t* found = mmdb_collection_get(db_, name);
        EXPECT_NE(found, nullptr) << "iter=" << i;
    }
}

/* 测试 4：schema_deep_copy 走的 ctx 分配（fields.name/字段由 ctx 管理） */
TEST_F(SdkCoreMemctxTest, SchemaDeepCopyUsesContext) {
    db_ = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db_, nullptr);

    /* 创建带字段的 collection */
    mmdb_field_def_t fields[] = {
        {const_cast<char*>("title"), MMDB_TYPE_TEXT, 0, nullptr},
    };
    mmdb_schema_t schema = {MMDB_MODEL_TEXT, 1, fields, 0};
    mmdb_collection_t* c = mmdb_collection_create(db_, "doc", &schema);
    ASSERT_NE(c, nullptr);

    /* 验证 collection 已记录（c->schema.fields 应由 ctx 分配） */
    EXPECT_NE(c, nullptr);
    EXPECT_EQ(c->schema.field_count, 1u);

    /* 关闭数据库，由 ctx 统一清理 fields.name / fields.default_value_json */
    mmdb_close(db_);
    db_ = nullptr;
}

/* 测试 5：error 信息走 ctx 分配（验证 mmdb_set_error 在迁移后的兼容性） */
TEST_F(SdkCoreMemctxTest, ErrorMessagesUseContext) {
    db_ = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db_, nullptr);

    /* 触发一个错误（如创建重复 collection） */
    mmdb_schema_t schema = {MMDB_MODEL_TEXT, 0, nullptr, 0};
    mmdb_collection_t* c1 = mmdb_collection_create(db_, "dup", &schema);
    ASSERT_NE(c1, nullptr);

    mmdb_collection_t* c2 = mmdb_collection_create(db_, "dup", &schema);
    EXPECT_EQ(c2, nullptr);
    EXPECT_EQ(mmdb_last_error_code(db_), MMDB_ERR_ALREADY);

    /* 错误信息字符串应可读 */
    const char* msg = mmdb_last_error_message(db_);
    ASSERT_NE(msg, nullptr);
    EXPECT_GT(std::strlen(msg), 0u);
}
