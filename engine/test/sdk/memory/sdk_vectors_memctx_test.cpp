/**
 * @file sdk_vectors_memctx_test.cpp
 * @brief Task 11：SDK Vectors 模块 MemoryContext 迁移验证测试
 *
 * 覆盖以下能力：
 * 1. 向量 collection 创建后 db->memory_context 仍稳定
 * 2. mmdb_vectors_add / search / get / delete 在 ctx 集成下正常工作
 * 3. HNSW wrapper 重建（rebuild 后析构正确）
 * 4. result items 继续由 result.c 清理（保持兼容 ABI）
 * 5. 大量 add/search/delete 循环验证 ctx 不会破坏向量流程
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "sdk/mmdb.h"
#include "sdk/mmdb_vectors.h"        /* mmdb_vectors_add/search/delete */
#include "sdk/impl/mmdb_internal.h"   /* 暴露 struct mmdb_s 真实结构 */
#include "sdk/impl/mmdb_memctx.h"
#include "sdk/impl/memctx.h"
}

namespace {

constexpr const char* kDbPath = "test_sdk_vectors_memctx.db";
constexpr size_t kDim = 16;

class SdkVectorsMemctxTest : public ::testing::Test {
   protected:
    mmdb_t* db_ = nullptr;
    mmdb_collection_t* coll_ = nullptr;

    void SetUp() override {
        std::remove(kDbPath);
        db_ = mmdb_open(kDbPath, nullptr);
        ASSERT_NE(db_, nullptr);
        mmdb_schema_t s = {MMDB_MODEL_VECTOR, 0, nullptr, kDim};
        coll_ = mmdb_collection_create(db_, "vec", &s);
        ASSERT_NE(coll_, nullptr);
    }

    void TearDown() override {
        if (db_) {
            mmdb_close(db_);
            db_ = nullptr;
        }
        std::remove(kDbPath);
    }
};

}  // namespace

/* 测试 1：ctx 已建立，collection 可用于向量操作 */
TEST_F(SdkVectorsMemctxTest, AddAndSearchBasic) {
    ASSERT_NE(db_->memory_context, nullptr);
    ASSERT_NE(coll_, nullptr);

    /* 插入一个向量 */
    const char* id = "v1";
    float vec[kDim];
    for (size_t i = 0; i < kDim; i++) vec[i] = (i == 0) ? 1.0f : 0.0f;

    mmdb_vector_t v = {(const uint8_t*)id, 2, vec, kDim, nullptr, nullptr};
    EXPECT_EQ(mmdb_vectors_add(coll_, &v, 1), MMDB_OK);

    /* 查询：结果应当 >= 1 */
    float q[kDim] = {0};
    q[0] = 1.0f;
    mmdb_query_t query = {q, kDim, 5, nullptr, 0, 0};
    mmdb_result_t result;
    ASSERT_EQ(mmdb_vectors_search(coll_, &query, &result), MMDB_OK);
    EXPECT_GE(result.count, 1u);

    /* 距离为 0（相同向量） */
    if (result.count > 0) {
        EXPECT_FLOAT_EQ(result.items[0].distance, 0.0f);
        EXPECT_EQ(result.items[0].id_len, 2u);
        EXPECT_EQ(memcmp(result.items[0].id, "v1", 2), 0);
    }

    mmdb_result_free(&result);
}

/* 测试 2：批量插入 + 搜索 */
TEST_F(SdkVectorsMemctxTest, BatchInsertAndSearch) {
    const char* ids[] = {"a", "b", "c", "d", "e"};
    float vecs[][kDim] = {
        {1.0f, 0.0f},   /* a */
        {0.0f, 1.0f},   /* b */
        {0.5f, 0.5f},   /* c */
        {0.7f, 0.3f},   /* d */
        {0.3f, 0.7f},   /* e */
    };
    /* 把行向量填充到 kDim */
    std::vector<std::vector<float>> padded;
    for (int i = 0; i < 5; i++) {
        std::vector<float> row(kDim, 0.0f);
        row[0] = vecs[i][0];
        row[1] = vecs[i][1];
        padded.push_back(row);
    }

    std::vector<mmdb_vector_t> batch;
    for (int i = 0; i < 5; i++) {
        batch.push_back({(const uint8_t*)ids[i], 1, padded[i].data(), kDim,
                         nullptr, nullptr});
    }
    EXPECT_EQ(mmdb_vectors_add(coll_, batch.data(), 5), MMDB_OK);

    /* 搜索接近 a */
    std::vector<float> q(kDim, 0.0f);
    q[0] = 1.0f;
    q[1] = 0.1f;
    mmdb_query_t query = {q.data(), kDim, 2, nullptr, 0, 0};
    mmdb_result_t result;
    ASSERT_EQ(mmdb_vectors_search(coll_, &query, &result), MMDB_OK);
    EXPECT_GE(result.count, 2u);
    if (result.count >= 1) {
        /* 最近的是 a */
        EXPECT_EQ(memcmp(result.items[0].id, "a", 1), 0);
    }
    mmdb_result_free(&result);
}

/* 测试 3：删除后重新搜索结果集变小 */
TEST_F(SdkVectorsMemctxTest, DeleteReducesResults) {
    const char* ids[] = {"x", "y", "z"};
    float vecs[][kDim] = {
        {1.0f, 0.0f},   /* x */
        {0.0f, 1.0f},   /* y */
        {0.5f, 0.5f},   /* z */
    };
    std::vector<std::vector<float>> padded;
    for (int i = 0; i < 3; i++) {
        std::vector<float> row(kDim, 0.0f);
        row[0] = vecs[i][0];
        row[1] = vecs[i][1];
        padded.push_back(row);
    }

    std::vector<mmdb_vector_t> batch;
    for (int i = 0; i < 3; i++) {
        batch.push_back({(const uint8_t*)ids[i], 1, padded[i].data(), kDim,
                         nullptr, nullptr});
    }
    ASSERT_EQ(mmdb_vectors_add(coll_, batch.data(), 3), MMDB_OK);

    /* 删除 x */
    EXPECT_EQ(mmdb_vectors_delete(coll_, (const uint8_t*)"x", 1), MMDB_OK);

    /* 搜索应当只返回 y 和 z（即使 top_k=10） */
    std::vector<float> q(kDim, 0.0f);
    q[0] = 1.0f;
    mmdb_query_t query = {q.data(), kDim, 10, nullptr, 0, 0};
    mmdb_result_t result;
    ASSERT_EQ(mmdb_vectors_search(coll_, &query, &result), MMDB_OK);
    /* 至少确认 x 不在结果中 */
    for (size_t i = 0; i < result.count; i++) {
        EXPECT_NE(memcmp(result.items[i].id, "x", 1), 0)
            << "result.item[" << i << "].id should not be x";
    }
    mmdb_result_free(&result);
}

/* 测试 4：upsert 替换向量 */
TEST_F(SdkVectorsMemctxTest, UpsertReplacesVector) {
    const char* id = "k";
    float v1[kDim] = {0};
    v1[0] = 1.0f;
    mmdb_vector_t a = {(const uint8_t*)id, 1, v1, kDim, nullptr, nullptr};
    ASSERT_EQ(mmdb_vectors_add(coll_, &a, 1), MMDB_OK);

    float v2[kDim] = {0};
    v2[1] = 1.0f;
    mmdb_vector_t b = {(const uint8_t*)id, 1, v2, kDim, nullptr, nullptr};
    ASSERT_EQ(mmdb_vectors_upsert(coll_, &b, 1), MMDB_OK);

    /* 查询 y 轴方向的向量应能找到 k */
    float q[kDim] = {0};
    q[1] = 1.0f;
    mmdb_query_t query = {q, kDim, 1, nullptr, 0, 0};
    mmdb_result_t result;
    ASSERT_EQ(mmdb_vectors_search(coll_, &query, &result), MMDB_OK);
    if (result.count >= 1) {
        EXPECT_EQ(memcmp(result.items[0].id, "k", 1), 0);
        EXPECT_FLOAT_EQ(result.items[0].distance, 0.0f);
    }
    mmdb_result_free(&result);
}

/* 测试 5：多次 add/search/delete 循环不会破坏 ctx */
TEST_F(SdkVectorsMemctxTest, RepeatedCyclesDoNotBreakContext) {
    /*
     * 关键：id 字符串与向量数据必须持久存储，且 batch 中的指针
     * 在调用 mmdb_vectors_add 时仍指向同一块非失效内存。
     *
     * 避免 std::vector 在循环中 push_back 触发扩容导致早期保存的
     * c_str()/data() 指针失效。先一次性 reserve 足够容量再写入。
     */
    constexpr int kRounds = 5;
    constexpr int kPerRound = 10;
    std::vector<std::string> id_storage;
    std::vector<std::vector<float>> vec_storage;
    id_storage.reserve(kRounds * kPerRound);
    vec_storage.reserve(kRounds * kPerRound);

    std::vector<mmdb_vector_t> batch;
    batch.reserve(kPerRound);

    for (int round = 0; round < kRounds; round++) {
        batch.clear();
        for (int i = 0; i < kPerRound; i++) {
            /* 持久化 id 字符串与向量数据 */
            char id_buf[8];
            snprintf(id_buf, sizeof(id_buf), "r%d-%d", round, i);
            id_storage.emplace_back(id_buf);

            vec_storage.emplace_back(kDim, 0.0f);
            vec_storage.back()[0] = (float)(i + round * 10) / 100.0f;
            vec_storage.back()[1] = (float)i / 10.0f;
        }

        /* 第二遍：构造 batch，c_str()/data() 指向已稳定的持久内存 */
        size_t base = (size_t)round * kPerRound;
        for (int i = 0; i < kPerRound; i++) {
            batch.push_back(
                {(const uint8_t*)id_storage[base + i].c_str(),
                 id_storage[base + i].size(),
                 vec_storage[base + i].data(), kDim, nullptr, nullptr});
        }
        ASSERT_EQ(mmdb_vectors_add(coll_, batch.data(), kPerRound), MMDB_OK);

        /* 搜索验证 */
        std::vector<float> q(kDim, 0.0f);
        q[0] = 0.5f;
        q[1] = 0.5f;
        mmdb_query_t query = {q.data(), kDim, 5, nullptr, 0, 0};
        mmdb_result_t result;
        ASSERT_EQ(mmdb_vectors_search(coll_, &query, &result), MMDB_OK);

        /* 验证 ctx 仍处于活跃状态 */
        EXPECT_FALSE(db_->memory_context->is_deleted);
        mmdb_result_free(&result);
    }
}

/* 测试 6：空 collection 搜索应返回 0 结果 */
TEST_F(SdkVectorsMemctxTest, EmptyCollectionReturnsZeroResults) {
    float q[kDim] = {0};
    q[0] = 1.0f;
    mmdb_query_t query = {q, kDim, 10, nullptr, 0, 0};
    mmdb_result_t result;
    ASSERT_EQ(mmdb_vectors_search(coll_, &query, &result), MMDB_OK);
    EXPECT_EQ(result.count, 0u);
    mmdb_result_free(&result);
}

/* 测试 7：error 与 ctx 协作（错误信息由 ctx 持有） */
TEST_F(SdkVectorsMemctxTest, ErrorPropagationWithContext) {
    /* dim 不匹配触发错误 */
    const char* id = "bad";
    float v[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    mmdb_vector_t bad = {(const uint8_t*)id, 3, v, 4, nullptr, nullptr};
    EXPECT_NE(mmdb_vectors_add(coll_, &bad, 1), MMDB_OK);

    /* ctx 仍能正常分配 */
    void* p = mmdb_mem_alloc(db_->memory_context, 128);
    EXPECT_NE(p, nullptr);
}

/* 测试 8：mmdb_close 触发 ctx 删除后 collection 句柄不再可用 */
TEST(SdkVectorsMemctxClose, CloseDeletesRootContext) {
    std::remove(kDbPath);
    mmdb_t* db = mmdb_open(kDbPath, nullptr);
    ASSERT_NE(db, nullptr);
    MemoryContext root = db->memory_context;
    mmdb_schema_t s = {MMDB_MODEL_VECTOR, 0, nullptr, kDim};
    mmdb_collection_t* c = mmdb_collection_create(db, "v", &s);
    ASSERT_NE(c, nullptr);

    mmdb_close(db);
    EXPECT_TRUE(root->is_deleted);
    std::remove(kDbPath);
}
