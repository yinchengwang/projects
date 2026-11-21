# 多模态嵌入式 SDK 核心库
cmake_minimum_required(VERSION 3.20)
project(mmsdk C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 依赖：SQLite（amalgamation，位于 engine/third_part/sqlite3/）
set(_sqlite3_dir "${CMAKE_CURRENT_LIST_DIR}/../../third_part/sqlite3")
if(EXISTS "${_sqlite3_dir}/sqlite3.c")
    if(NOT TARGET sqlite3)
        add_library(sqlite3 STATIC "${_sqlite3_dir}/sqlite3.c")
        target_include_directories(sqlite3 PUBLIC "${_sqlite3_dir}")
        if(MSVC)
            target_compile_options(sqlite3 PRIVATE /W0)
        else()
            target_compile_options(sqlite3 PRIVATE -w)
        endif()
    endif()
else()
    message(FATAL_ERROR "engine/third_part/sqlite3/sqlite3.c 不存在，请下载 SQLite amalgamation")
endif()

# 核心模块
set(SDK_CORE_SOURCES
    core/error.c
    core/sqlite_backend.c
    core/mmdb.c
    core/schema.c
    core/collection.c
    core/filter_parser.c
    core/result.c
    core/metrics.c   # P6-M1.2：运行时监控指标
    core/txn.c       # P6-M2.2：ACID 事务
    core/backup.c    # P6-M2.1：数据库备份/恢复
    core/replication.c  # P6-M3.1：Raft 复制协议
    core/sharding.c  # P6-M3.2：一致性哈希分片
    core/namespace.c  # P6-M3.3：命名空间隔离与资源配额
    core/mmdb_memctx.c  # SDK 兼容层内存上下文
    core/memctx.c        # SDK 自持的 AllocSet 实现（脱离冻结内核）
    core/mmdb_lock.c
)

# 向量模块
set(SDK_VECTORS_SOURCES
    vectors/vectors.c
    vectors/index_flat.c
    vectors/vectors_sql.c
)

# 图模块
set(SDK_GRAPH_SOURCES
    graph/graph.c
    graph/graph_traverse.c
    graph/graph_sql.c
)

# 时序模块
set(SDK_TIMESERIES_SOURCES
    timeseries/timeseries.c
    timeseries/agg.c
    timeseries/timeseries_sql.c
)

# 文本模块
set(SDK_TEXT_SOURCES
    text/text.c
    text/text_fts5.c
    text/text_sql.c
)

# 跨模型模块（P2 占位 + P3 混合检索）
set(SDK_EXTRA_SOURCES
    extra/cross_model.c
    extra/rag.c
    extra/rrf.c   # P3-T1.1 新增：RRF 多通道融合
    extra/hybrid_search.c   # P3-T1.2 新增：hybrid search 公共 API（向量+FTS5+RRF）
    extra/embedding_local.c   # P3-T2.1 新增：embedding 接口抽象 + hash/average-pool/openai 占位
    extra/embedding_openai.c   # P3-T2.2 新增：OpenAI adapter 占位
    extra/xquery.c   # P3-T3.1 新增：跨 collection join（text→vector）接口与实现
)

# P6-M4.1：通用聚合框架
set(SDK_AGGREGATE_SOURCES
    aggregation/aggregate.c
)

# 创建静态库
add_library(mmsdk STATIC
    ${SDK_CORE_SOURCES}
    ${SDK_VECTORS_SOURCES}
    ${SDK_GRAPH_SOURCES}
    ${SDK_TIMESERIES_SOURCES}
    ${SDK_TEXT_SOURCES}
    ${SDK_EXTRA_SOURCES}
    ${SDK_AGGREGATE_SOURCES}
)

# 公共包含路径（必须在 add_library 之后）
target_include_directories(mmsdk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/..  # Task 3 完成前仍需 HNSW 头文件
)
# P5-2: 添加 third_part 路径，用于 roaring_bitmap.h
# 注：原先路径上溯三级指向仓库根目录，已改用 engine/third_part/croaring（内部副本），
#     不再依赖仓库根目录下的 third_part/ 布局。
target_include_directories(mmsdk PRIVATE
    ${ENGINEERING_SOURCE_DIR}/third_part/croaring
)

# Task 2 只摘 sql_engine（它提供的 memctx 符号已由 core/memctx.c 本地提供）。
# faiss_hnsw / mm_vector_index 属于 Task 3 的向量后端替换范围，本 Task 先留着。
target_link_libraries(mmsdk PUBLIC sqlite3 faiss_hnsw mm_vector_index)

# 编译选项
if(MSVC)
    target_compile_options(mmsdk PRIVATE /W4)
else()
    target_compile_options(mmsdk PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# 安装规则
install(TARGETS mmsdk
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../include/sdk/
    DESTINATION include/sdk
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)