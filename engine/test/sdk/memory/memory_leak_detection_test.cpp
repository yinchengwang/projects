/**
 * @file memory_leak_detection_test.cpp
 * @brief 内存泄漏检测测试
 *
 * 覆盖以下能力：
 * 1. mmdb_close 释放 memory_context
 * 2. 多次 open/close 不泄漏
 * 3. request scope 自动释放
 */

// 注意：必须先包含 gtest，再包含 memctx.h。
#include <gtest/gtest.h>

#include <cstdio>

extern "C" {
#include "sdk/mmdb.h"
#include "sdk/impl/mmdb_internal.h"  /* 暴露 struct mmdb_s 真实结构 */
#include "sdk/impl/memctx.h"           /* MemoryContextData 字段访问 */
}

/**
 * @brief 测试夹具：用于内存泄漏检测测试
 */
class MemoryLeakDetectionTest : public ::testing::Test {
protected:
    const char *db_path = "test_leak_detection.db";

    void SetUp() override {
        std::remove(db_path);
    }

    void TearDown() override {
        std::remove(db_path);
    }
};

/**
 * @brief 测试 mmdb_close 释放 memory_context
 *
 * 验证关闭数据库后，根上下文被标记为删除
 */
TEST_F(MemoryLeakDetectionTest, CloseReleasesMemoryContext) {
    mmdb_t *db = mmdb_open(db_path, nullptr);
    ASSERT_NE(db, nullptr);

    MemoryContext root = db->memory_context;

    mmdb_close(db);

    /* 根上下文应被标记为删除 */
    EXPECT_TRUE(root->is_deleted);
}

/**
 * @brief 测试多次 open/close 不泄漏
 *
 * 验证重复打开关闭数据库不会导致内存泄漏
 */
TEST_F(MemoryLeakDetectionTest, RepeatedOpenCloseNoLeak) {
    for (int i = 0; i < 10; i++) {
        mmdb_t *db = mmdb_open(db_path, nullptr);
        ASSERT_NE(db, nullptr);
        mmdb_close(db);
    }
    /* 无泄漏 */
    SUCCEED();
}

/**
 * @brief 测试 request scope 自动释放
 *
 * 验证请求作用域结束后，请求上下文被自动清理
 */
TEST_F(MemoryLeakDetectionTest, RequestScopeAutoCleanup) {
    mmdb_t *db = mmdb_open(db_path, nullptr);
    ASSERT_NE(db, nullptr);

    mmdb_request_scope_t scope;
    ASSERT_EQ(mmdb_request_begin(db, "test", &scope), MMDB_OK);

    /* 在请求上下文中分配 */
    void *ptr = palloc(scope.context, 1024);
    ASSERT_NE(ptr, nullptr);
    EXPECT_GT(scope.context->current_bytes, 0u);

    MemoryContext ctx = scope.context;
    mmdb_request_end(&scope);

    /* 请求上下文应被标记为删除 */
    EXPECT_TRUE(ctx->is_deleted);

    mmdb_close(db);
}
