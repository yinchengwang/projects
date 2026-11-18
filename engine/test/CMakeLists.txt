# engine/test/CMakeLists.txt
# 工程层测试入口（仅 C++ 工程代码 + 数据库内核）
# 双轨纪律：engine/test 不应测 ds/、leetcode/、interview/ 库（这些已随学习轨迁至独立仓 yinchengwang/learning）

cmake_minimum_required(VERSION 3.20)

# 启用测试
enable_testing()

# 工程层测试子目录（按子目录细分）
# 说明：S6 起移除 self_made/、leetcode/、interview/ 测试目录（被测库已迁移到 learning/）
# 工程层测试子目录（按子目录细分）
# 说明：S6 起移除 self_made/、leetcode/、interview/ 测试目录（被测库已迁移到 learning/）

add_subdirectory(algo)             # 算法测试（algo-prod）
add_subdirectory(self_made_cpp)   # C++ 工程代码（trie）

# ========================================================================
# 冻结内核的测试：默认不构建、不注册（B5，见 engine/CMakeLists.txt 的
# ENABLE_FROZEN_SRC_DB_TESTS 说明）。engine/src/db 已于 2026-11-16 冻结，
# 这 289 个源文件 / 约 587 条 ctest 用例测的是不再演进的内核。
#
# 注意 db/consensus 不挂在 db/CMakeLists.txt 里，是从这一层单独挂的，
# 所以两处都要守卫，否则会漏掉 raft / raft_cluster 等 5 个用例。
# ========================================================================
if(ENABLE_FROZEN_SRC_DB_TESTS)
    add_subdirectory(db)
    add_subdirectory(db/consensus)   # Raft 共识模块测试（Phase11）
    # add_subdirectory(db/index/vector_index)  # 向量索引测试（暂时跳过，add_subdirectory 冲突）
endif()

add_subdirectory(kbase)            # kbase 知识库测试
add_subdirectory(sdk)            # 多模态嵌入式 SDK测试
add_subdirectory(sdk/integration) # 跨语言一致性 + 性能基准
add_subdirectory(smoke)           # 模态独立冒烟（Task 7 起，每模态一个）

# 创建一个聚合测试目标
add_custom_target(all_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running all engineering tests..."
    VERBATIM
)

# all_tests 目标依赖：每个测试 target 都自动加入（除了子目录 internal target）
set(_test_targets
    raft_test
    self_made_cpp_test
    test_db
    test_mvcc_test
    test_optimizer_test
    test_replication_test
    test_sql
    test_storage_all
    test_concurrency
    test_hilbert
    test_mm_pool
    test_mvcc
    test_optimizer
    test_replication
    test_rest_api
    test_sla
    test_sql_parser
    test_storage_kv
    test_storage_kv_ttl
    test_storage_main
    test_storage_wal
    test_vector_api
    test_vector_persist
    test_vector_query
    vector_delete_test
    vector_reranker_test
    mmdb_core_test
    mmdb_vectors_test
    mmdb_graph_test
    mmdb_timeseries_test
    mmdb_text_test
    mmdb_filter_test
    cross_lang_consistency_test
    distributed_txn_test
    vectorized_exec_test
)

foreach(_t IN LISTS _test_targets)
    if(TARGET ${_t})
        add_dependencies(all_tests ${_t})
    endif()
endforeach()
