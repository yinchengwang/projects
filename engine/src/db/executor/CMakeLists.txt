# executor 模块：执行层
# 依赖 parser, optimizer, storage, validator
add_subdirectory(operators)
add_subdirectory(framework)
file(GLOB_RECURSE EXECUTOR_SOURCE_FILES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "*.c")

# sql/ 子目录仅含 canonical sqlExecutor.c（sqlExec.c 已归档）；
# RELATIONAL 关闭时整个 sql/ 子目录不编译
if(NOT MMDB_ENABLE_RELATIONAL)
    list(FILTER EXECUTOR_SOURCE_FILES EXCLUDE REGEX ".*/sql/.*\\.c$")
endif()

if(EXECUTOR_SOURCE_FILES)
    add_library(db_executor STATIC ${EXECUTOR_SOURCE_FILES})
    target_include_directories(db_executor PUBLIC
        ${ENGINEERING_SOURCE_DIR}/include
        ${ENGINEERING_SOURCE_DIR}/include/db
        ${ENGINEERING_SOURCE_DIR}/include/db/executor
        ${ENGINEERING_SOURCE_DIR}/include/db/parser
        ${ENGINEERING_SOURCE_DIR}/include/db/optimizer
        ${ENGINEERING_SOURCE_DIR}/include/db/storage
        ${ENGINEERING_SOURCE_DIR}/include/db/validator
        ${ENGINEERING_SOURCE_DIR}/include/db/core
    )
    # db_parser 父目标在 sql/ 子模块存在时不会创建（见 parser/CMakeLists.txt）。
    # T4：db_parser_sql_simplified 已归档移除；T6 落地 canonical 目标 db_parser_sql。
    # db_executor 当前不直接引用解析器符号，无需链接 db_parser_sql；
    # 若后续 TU 调用 sql_parse()/makefuncs，请在此追加 db_parser_sql。
    # T11：RELATIONAL 关闭时 db_parser_sql 不存在，需 if(TARGET) 守卫。
    if(TARGET db_parser_sql)
        target_link_libraries(db_executor PRIVATE db_parser_sql)
    endif()
    # Task 1: vector_block_*/vector_batch_* 已从 db_core 迁入 db_vectorized
    target_link_libraries(db_executor PRIVATE db_optimizer db_storage db_validator db_vectorized)
    target_compile_options(db_executor PRIVATE -Wno-unused-parameter)
endif()

# Task 15：RAG 执行器拆为独立库并挂 VECTOR 守卫
# C9-3：rag/*.c 自包含（仅依赖 rag_executor.h / graphrag.h / vector_engine.h）；
# db_executor 本体不含 rag/、不引用 vector_engine.h（grep 可证）。
if(MMDB_ENABLE_VECTOR)
    add_library(mm_executor_rag STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/rag/rag_executor.c
        ${CMAKE_CURRENT_SOURCE_DIR}/rag/rag_pipeline.c
        ${CMAKE_CURRENT_SOURCE_DIR}/rag/chunk_op.c
        ${CMAKE_CURRENT_SOURCE_DIR}/rag/embed_op.c
        ${CMAKE_CURRENT_SOURCE_DIR}/rag/retrieve_op.c
        ${CMAKE_CURRENT_SOURCE_DIR}/rag/generate_op.c
        ${CMAKE_CURRENT_SOURCE_DIR}/rag/graphrag_context.c
        ${CMAKE_CURRENT_SOURCE_DIR}/rag/graphrag_entity.c
        ${CMAKE_CURRENT_SOURCE_DIR}/rag/graphrag_hybrid_search.c
        ${CMAKE_CURRENT_SOURCE_DIR}/rag/graphrag_relation.c
    )
    target_include_directories(mm_executor_rag PUBLIC
        ${ENGINEERING_SOURCE_DIR}/include
        ${ENGINEERING_SOURCE_DIR}/include/db
        ${ENGINEERING_SOURCE_DIR}/include/db/executor
    )
    target_link_libraries(mm_executor_rag PUBLIC mm_vector)
    set_target_properties(mm_executor_rag PROPERTIES FOLDER "ModalityEngines")
endif()

# 这个用例的源码在 engine/test/db/executor/ 下，却由 src 侧自己注册 ——
# 所以摘 test/CMakeLists.txt 的 add_subdirectory(db) 摘不掉它。
# 必须跟着同一个开关走（B5，见 engine/CMakeLists.txt）。
if(BUILD_TESTING AND ENABLE_FROZEN_SRC_DB_TESTS)
    add_executable(test_rag_executor
        ${ENGINEERING_SOURCE_DIR}/test/db/executor/rag_executor_test.cpp
    )
    target_include_directories(test_rag_executor PRIVATE
        ${ENGINEERING_SOURCE_DIR}/include
        ${ENGINEERING_SOURCE_DIR}/include/db
        ${ENGINEERING_SOURCE_DIR}/include/db/executor
        ${THIRD_PART_DIR}/googletest/googletest/include
        ${THIRD_PART_DIR}/googletest/googlemock/include
    )
    if(TARGET mm_executor_rag)
        target_link_libraries(test_rag_executor PRIVATE mm_executor_rag)
    endif()
    target_link_libraries(test_rag_executor PRIVATE
        db_core
        project_includes
        gtest
        gtest_main
    )
    add_test(NAME rag_executor_test COMMAND test_rag_executor)
endif()
