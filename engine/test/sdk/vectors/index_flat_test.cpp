/*
 * index_flat_test.cpp —— SDK 自持精确扁平索引单元测试
 *
 * 验证索引自身的构造、容量增长、批量追加、KNN 排序与过滤语义；
 * SDK 公共向量 API 的集成覆盖仍由 mmdb_vectors_test 承担。
 */
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

extern "C" {
#include "sdk/impl/index_flat.h"
}

TEST(FlatIndex, CreateAndDrop)
{
    auto *idx = mmdb_flat_index_create(4);
    ASSERT_NE(idx, nullptr);
    EXPECT_EQ(mmdb_flat_index_size(idx), 0);
    mmdb_flat_index_drop(idx);
    mmdb_flat_index_drop(nullptr);
    EXPECT_EQ(mmdb_flat_index_create(0), nullptr);
    EXPECT_EQ(mmdb_flat_index_create(-1), nullptr);
}

TEST(FlatIndex, ReserveAndAdd)
{
    auto *idx = mmdb_flat_index_create(2);
    ASSERT_NE(idx, nullptr);
    ASSERT_EQ(mmdb_flat_index_reserve(idx, 3), 0);
    const float vectors[] = {0, 0, 1, 1, 2, 2};
    EXPECT_EQ(mmdb_flat_index_add(idx, 3, vectors), 3);
    EXPECT_EQ(mmdb_flat_index_size(idx), 3);
    EXPECT_EQ(mmdb_flat_index_add(idx, 0, vectors), -1);
    mmdb_flat_index_drop(idx);
}

TEST(FlatIndex, SearchReturnsNearestFirst)
{
    auto *idx = mmdb_flat_index_create(2);
    ASSERT_NE(idx, nullptr);
    const float vectors[] = {0, 0, 10, 10, 1, 1};
    ASSERT_EQ(mmdb_flat_index_add(idx, 3, vectors), 3);
    const float query[] = {0, 0};
    int32_t ids[3] = {};
    float distances[3] = {};

    ASSERT_EQ(mmdb_flat_search_filtered(idx, query, 3, nullptr, nullptr,
                                        ids, distances), 3);
    EXPECT_EQ(ids[0], 0);
    EXPECT_EQ(ids[1], 2);
    EXPECT_EQ(ids[2], 1);
    EXPECT_NEAR(distances[0], 0.0f, 1e-6f);
    EXPECT_NEAR(distances[1], 2.0f, 1e-6f);
    EXPECT_NEAR(distances[2], 200.0f, 1e-6f);
    mmdb_flat_index_drop(idx);
}

static int reject_even(int32_t vec_id, void *)
{
    return (vec_id % 2) != 0;
}

TEST(FlatIndex, FilterExcludesCandidates)
{
    auto *idx = mmdb_flat_index_create(2);
    ASSERT_NE(idx, nullptr);
    const float vectors[] = {0, 0, 10, 10, 1, 1};
    ASSERT_EQ(mmdb_flat_index_add(idx, 3, vectors), 3);
    const float query[] = {0, 0};
    int32_t ids[3] = {};
    float distances[3] = {};

    ASSERT_EQ(mmdb_flat_search_filtered(idx, query, 3, reject_even, nullptr,
                                        ids, distances), 1);
    EXPECT_EQ(ids[0], 1);
    EXPECT_NEAR(distances[0], 200.0f, 1e-6f);
    mmdb_flat_index_drop(idx);
}
