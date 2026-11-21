// mmdb_memctx_test.cpp — Task 7：SDK 兼容层 mmdb_mem_* API 测试
//
// 覆盖以下能力：
// 1. mmdb_memctx_create：创建带限额/不带限额上下文
// 2. mmdb_mem_alloc/calloc/realloc/strdup/free：标准分配 API
// 3. mmdb_mem_calloc：溢出保护
// 4. mmdb_mem_alloc：限额分配失败语义
// 5. mmdb_mem_register_resource：资源析构回调在 reset 时触发
// 6. mmdb_memctx_reset / delete：生命周期管理

// 注意：必须先包含 gtest，再包含 memctx.h。
#include <gtest/gtest.h>

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>

extern "C" {
#include "sdk/impl/mmdb_memctx.h"
}

namespace {

// 测试夹具：默认创建一个不限额的根上下文
class MmdbMemctxTest : public ::testing::Test {
   protected:
    mmdb_memctx_t ctx = nullptr;

    void SetUp() override {
        ctx = mmdb_memctx_create(nullptr, "sdk-memctx-test", 0);
        ASSERT_NE(ctx, nullptr);
    }

    void TearDown() override {
        if (ctx) {
            mmdb_memctx_delete(ctx);
            ctx = nullptr;
        }
    }
};

}  // namespace

// 测试 1：基础分配与重分配
TEST_F(MmdbMemctxTest, AllocCallocReallocAndStrdup) {
    // alloc：分配 32 字节并写入
    void *raw = mmdb_mem_alloc(ctx, 32);
    ASSERT_NE(raw, nullptr);
    memset(raw, 0xAB, 32);

    // calloc：分配 8 个 int 并验证清零
    int *values = static_cast<int *>(mmdb_mem_calloc(ctx, 8, sizeof(int)));
    ASSERT_NE(values, nullptr);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(values[i], 0);
    }

    // strdup：复制字符串
    char *text = mmdb_mem_strdup(ctx, "hello");
    ASSERT_NE(text, nullptr);
    EXPECT_STREQ(text, "hello");

    // realloc：扩容并保留内容
    text = static_cast<char *>(mmdb_mem_realloc(ctx, text, 64));
    ASSERT_NE(text, nullptr);
    EXPECT_STREQ(text, "hello");

    // free：AllocSet 下为空操作，不能崩溃
    mmdb_mem_free(ctx, raw);
    mmdb_mem_free(ctx, values);
    mmdb_mem_free(ctx, text);
}

// 测试 2：calloc 溢出保护
TEST_F(MmdbMemctxTest, CallocOverflowReturnsNull) {
    // count * size 溢出 SIZE_MAX 时必须返回 NULL
    EXPECT_EQ(mmdb_mem_calloc(ctx, SIZE_MAX, 2), nullptr);
    // count 为 0 也返回 NULL
    EXPECT_EQ(mmdb_mem_calloc(ctx, 0, 16), nullptr);
    // size 为 0 也返回 NULL
    EXPECT_EQ(mmdb_mem_calloc(ctx, 16, 0), nullptr);
}

// 测试 3：realloc 边界（NULL 等价于 alloc，size=0 等价于 free）
TEST_F(MmdbMemctxTest, ReallocNullAndZeroSize) {
    // ptr == NULL 应走 alloc 路径
    void *p = mmdb_mem_realloc(ctx, nullptr, 64);
    ASSERT_NE(p, nullptr);

    // size == 0 应走 free 路径，返回 NULL
    void *q = mmdb_mem_realloc(ctx, p, 0);
    EXPECT_EQ(q, nullptr);
}

// 测试 4：限额生效（超出 max_bytes 的分配返回 NULL）
TEST_F(MmdbMemctxTest, LimitIsMappedToAllocationFailure) {
    mmdb_memctx_t limited = mmdb_memctx_create(ctx, "limited-child", 1024);
    ASSERT_NE(limited, nullptr);

    // 限额 1024 字节，512 字节可分配
    void *small = mmdb_mem_alloc(limited, 512);
    EXPECT_NE(small, nullptr);

    // 600 字节会触发限额失败（当前已用 512，余量不足以分配 600）
    EXPECT_EQ(mmdb_mem_alloc(limited, 600), nullptr);

    mmdb_memctx_delete(limited);
}

// 测试 5：资源析构在 reset 时触发（LIFO 顺序）
TEST_F(MmdbMemctxTest, ResourceDestructorRunsOnReset) {
    static int destroyed_count = 0;
    destroyed_count = 0;

    auto destructor = [](void * /*resource*/, void * /*arg*/) {
        ++destroyed_count;
    };

    int resource_a = 1;
    int resource_b = 2;

    // 注册两个资源
    EXPECT_EQ(mmdb_mem_register_resource(ctx, &resource_a, destructor,
                                         nullptr, "res-a"),
              0);
    EXPECT_EQ(mmdb_mem_register_resource(ctx, &resource_b, destructor,
                                         nullptr, "res-b"),
              0);

    // reset 触发 LIFO 析构
    mmdb_memctx_reset(ctx);

    EXPECT_EQ(destroyed_count, 2);

    // 重复 reset 不应再调用（资源已随 reset 释放）
    mmdb_memctx_reset(ctx);
    EXPECT_EQ(destroyed_count, 2);
}

// 测试 6：unregister_resource 取消注册
TEST_F(MmdbMemctxTest, UnregisterResourcePreventsDestructorCall) {
    static int destroyed_count = 0;
    destroyed_count = 0;

    auto destructor = [](void * /*resource*/, void * /*arg*/) {
        ++destroyed_count;
    };

    int resource = 42;
    EXPECT_EQ(mmdb_mem_register_resource(ctx, &resource, destructor,
                                         nullptr, "res-x"),
              0);

    // 取消注册
    EXPECT_EQ(mmdb_mem_unregister_resource(ctx, &resource), 0);

    // reset 不应再调用析构
    mmdb_memctx_reset(ctx);
    EXPECT_EQ(destroyed_count, 0);
}

// 测试 7：空参数校验（NULL 安全）
TEST_F(MmdbMemctxTest, NullArgumentValidation) {
    // alloc 空上下文
    EXPECT_EQ(mmdb_mem_alloc(nullptr, 32), nullptr);
    // alloc 0 字节
    EXPECT_EQ(mmdb_mem_alloc(ctx, 0), nullptr);

    // free 空指针安全
    mmdb_mem_free(ctx, nullptr);
    mmdb_mem_free(nullptr, reinterpret_cast<void *>(0x1));  // 不应崩溃

    // reset/delete 空上下文安全
    mmdb_memctx_reset(nullptr);
    mmdb_memctx_delete(nullptr);

    // strdup 空字符串指针
    EXPECT_EQ(mmdb_mem_strdup(ctx, nullptr), nullptr);

    // 注册资源空参数
    int r = 0;
    auto dtor = [](void *, void *) {};
    EXPECT_EQ(mmdb_mem_register_resource(nullptr, &r, dtor, nullptr, "x"), -1);
    EXPECT_EQ(mmdb_mem_register_resource(ctx, nullptr, dtor, nullptr, "x"), -1);
    EXPECT_EQ(mmdb_mem_register_resource(ctx, &r, nullptr, nullptr, "x"), -1);

    // 注销资源空参数
    EXPECT_EQ(mmdb_mem_unregister_resource(nullptr, &r), -1);
    EXPECT_EQ(mmdb_mem_unregister_resource(ctx, nullptr), -1);
}

// 测试 8：create 参数校验
TEST(MmdbMemctxCreate, NullNameReturnsNull) {
    // name 为空必须返回 NULL
    EXPECT_EQ(mmdb_memctx_create(nullptr, nullptr, 0), nullptr);
}

// 测试 9：父子上下文关系
TEST_F(MmdbMemctxTest, ParentChildHierarchy) {
    mmdb_memctx_t child = mmdb_memctx_create(ctx, "child", 0);
    ASSERT_NE(child, nullptr);

    // 子上下文可独立分配
    void *p = mmdb_mem_alloc(child, 64);
    EXPECT_NE(p, nullptr);

    // 删除子上下文不影响父上下文
    mmdb_memctx_delete(child);

    // 父上下文仍可用
    void *q = mmdb_mem_alloc(ctx, 32);
    EXPECT_NE(q, nullptr);
}
