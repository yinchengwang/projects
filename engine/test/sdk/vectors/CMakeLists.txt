# SDK 内存向量索引（精确扁平索引）测试
add_project_test(index_flat_test VAR
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/index_flat_test.cpp)
target_link_libraries(index_flat_test PRIVATE mmsdk project_includes)
