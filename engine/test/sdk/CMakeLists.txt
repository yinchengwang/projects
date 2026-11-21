# SDK 测试构建配置
cmake_minimum_required(VERSION 3.20)

# 注意：googletest 已被 engine/CMakeLists.txt 添加，gtest target 在父作用域可见
# 注意：src/sdk 已被 src/CMakeLists.txt 添加，mmsdk target 在父作用域可见

# 引入子目录（Task 7：SDK 兼容层 mmdb_mem_* API 测试）
add_subdirectory(memory)
add_subdirectory(vectors)  # SDK 自持精确扁平向量索引测试

# 测试用例列表
set(SDK_TESTS
    mmdb_core_test
    mmdb_vectors_test
    mmdb_graph_test
    mmdb_timeseries_test
    mmdb_text_test
    mmdb_filter_test
    mmdb_raii_test
    rrf_test   # P3-T1.1：RRF 融合测试，源码位于 hybrid/ 子目录
    hybrid_search_test   # P3-T1.2：hybrid search 公共 API 测试，源码位于 hybrid/ 子目录
    embedding_test   # P3-T2.1：embedding 接口测试，源码位于 embedding/ 子目录
    xquery_test   # P3-T3.1：跨 collection join（text→vector）测试，源码位于 xquery/ 子目录
    xquery_id_len_test   # P4-T4.3：id 超长升级为 MMDB_ERR_INVALID 测试，源码位于 xquery/ 子目录
    rag_test   # P3-T4.1：RAG retrieve 测试，源码位于 rag/ 子目录
    rerank_test   # P3-T4.2：RAG BM25 rerank 占位测试，源码位于 rag/ 子目录
    rag_embedding_test   # P4-T4.1：RAG embedding 配置入口测试，源码位于 rag/ 子目录
    hybrid_dual_channel_test   # P4-T4.2：双通道真正融合测试，源码位于 hybrid/ 子目录
    hybrid_dual_channel_enabled_test   # P5-6：双模同集合 SDK 架构测试，源码位于 hybrid/ 子目录
)

# 为每个测试源文件创建可执行文件
foreach(test_name IN LISTS SDK_TESTS)
    # rrf_test / hybrid_search_test 源码位于 hybrid/ 子目录
    # embedding_test 源码位于 embedding/ 子目录
    # xquery_test 源码位于 xquery/ 子目录
    # rag_test 源码位于 rag/ 子目录
    if(test_name STREQUAL "rrf_test" OR test_name STREQUAL "hybrid_search_test" OR test_name STREQUAL "hybrid_dual_channel_test" OR test_name STREQUAL "hybrid_dual_channel_enabled_test")
        add_executable(${test_name} hybrid/${test_name}.cpp)
    elseif(test_name STREQUAL "embedding_test")
        add_executable(${test_name} embedding/${test_name}.cpp)
    elseif(test_name STREQUAL "xquery_test" OR test_name STREQUAL "xquery_id_len_test")
        add_executable(${test_name} xquery/${test_name}.cpp)
    elseif(test_name STREQUAL "rag_test" OR test_name STREQUAL "rerank_test" OR test_name STREQUAL "rag_embedding_test")
        add_executable(${test_name} rag/${test_name}.cpp)
    else()
        add_executable(${test_name} ${test_name}.cpp)
    endif()
    target_link_libraries(${test_name} PRIVATE mmsdk gtest_main)
    target_include_directories(${test_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
    # 输出到 build 目录（不污染源码）
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/sdk_tests
    )
endforeach()