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

add_subdirectory(kbase)            # kbase 知识库测试
add_subdirectory(sdk)            # 多模态嵌入式 SDK测试
add_subdirectory(sdk/integration) # 跨语言一致性 + 性能基准

# 创建一个聚合测试目标
add_custom_target(all_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running all engineering tests..."
    VERBATIM
)

# all_tests 目标依赖：每个测试 target 都自动加入（除了子目录 internal target）
set(_test_targets
    self_made_cpp_test
    test_hilbert
    mmdb_core_test
    mmdb_vectors_test
    mmdb_graph_test
    mmdb_timeseries_test
    mmdb_text_test
    mmdb_filter_test
    cross_lang_consistency_test
    vectorized_exec_test
)

foreach(_t IN LISTS _test_targets)
    if(TARGET ${_t})
        add_dependencies(all_tests ${_t})
    endif()
endforeach()
