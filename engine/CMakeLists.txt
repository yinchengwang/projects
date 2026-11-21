# engine/CMakeLists.txt
# 工程轨道 CMake 入口（PG 风格存储、MiniVecDB、rag/、sdk/）
# 由根 CMakeLists.txt 在 ENGINEERING_BUILD=ON 时 add_subdirectory 引入
# 学习轨已切出为独立仓（yinchengwang/learning），本轨道不再引用其任何路径。

cmake_minimum_required(VERSION 3.20)

# GNUInstallDirs 提供 CMAKE_INSTALL_INCLUDEDIR / CMAKE_INSTALL_BINDIR 等默认路径
include(GNUInstallDirs)

project(AlgorithmPractice
    VERSION 1.0.0
    DESCRIPTION "Algorithm and Data Structure Practice Project (Engineering Track)"
    LANGUAGES C CXX
)

# 设置 C 和 C++ 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 启用文件夹组织

# 定义 ENGINEERING_SOURCE_DIR 宏，供子模块代替 CMAKE_SOURCE_DIR 引用头文件
# engine/include/ 下的头文件应在 project_includes 中已覆盖
# 但部分旧 CMakeLists 直接用 CMAKE_SOURCE_DIR/include/db/...
# 统一替换为 ENGINEERING_SOURCE_DIR/include/
# 注意：CMAKE_SOURCE_DIR 是根目录（book/），ENGINEERING_SOURCE_DIR 是 engine/
set(ENGINEERING_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ========================================================================
# 构建优化配置
# ========================================================================

# 使用 ccache 加速重复编译（如果安装了 ccache）
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
endif()

# 设置并行编译（GCC/MinGW 使用 -j，MSVC 使用 /MP）
include(ProcessorCount)
ProcessorCount(N_CORES)
if(N_CORES EQUAL 0)
    set(N_CORES 4)
endif()
set(CMAKE_BUILD_PARALLEL_LEVEL ${N_CORES})


include(CTest)
# include(GNUInstallDirs)

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(BUILD_TESTING "Build tests" ON)
option(ENABLE_COVERAGE "Enable code coverage" OFF)


if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

if(MSVC)
    add_compile_options(/W4 /WX)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    # GCC/Clang（与 AGENTS.md 第 54 行保持一致；不启用 -Werror，避免阻断既有 warning）
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-sign-compare)

    if(ENABLE_COVERAGE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-fprofile-arcs -ftest-coverage)
        add_link_options(-fprofile-arcs -ftest-coverage)
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(NOT MSVC)
        add_compile_options(-O0 -g3)
    endif()
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(NOT MSVC)
        add_compile_options(-O2 -DNDEBUG)
    endif()
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    if(NOT MSVC)
        add_compile_options(-O2 -g3 -DNDEBUG)
    endif()
elseif(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    if(NOT MSVC)
        add_compile_options(-Os -DNDEBUG)
    endif()
endif()

find_package(Threads REQUIRED)

# 加载工程工具函数（add_project_library / add_project_test）
include("${CMAKE_CURRENT_LIST_DIR}/cmake/ProjectUtils.cmake")

# 包含 cJSON（单文件 JSON 库）
# 注意：cjson 属于 engine/ 内部的 third_part/，随工程目录整体迁移，路径不变。
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/third_part/cjson/CMakeLists.txt")
    add_subdirectory(third_part/cjson ${CMAKE_BINARY_DIR}/third_part/cjson_build)
endif()

# 包含 libmicrohttpd（HTTP 服务器库）
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../vendor/third_part/libmicrohttpd/CMakeLists.txt")
    add_subdirectory(../vendor/third_part/libmicrohttpd ${CMAKE_BINARY_DIR}/third_part/libmicrohttpd_build)
endif()

if(BUILD_TESTING AND NOT TARGET gtest)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../vendor/third_part/googletest/CMakeLists.txt")
        add_subdirectory(../vendor/third_part/googletest ${CMAKE_BINARY_DIR}/third_part/googletest_build)
    else()
        find_package(GTest REQUIRED)
    endif()
endif()

# 创建接口库用于公共包含目录
add_library(project_includes INTERFACE)
target_include_directories(project_includes
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)


add_subdirectory(src)

# tools 工具（VectorBench benchmark 等）
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/tools/CMakeLists.txt")
    add_subdirectory(tools)
endif()

if(BUILD_TESTING)
    add_subdirectory(test)
endif()

# 主程序目标
set(_main_c "${CMAKE_CURRENT_LIST_DIR}/main.c")
if(EXISTS "${_main_c}")
    add_executable(${PROJECT_NAME} "${_main_c}")
    target_link_libraries(${PROJECT_NAME}
        PRIVATE
            project_includes
            ds
            Threads::Threads
    )

    # 编译产物统一进入 build/ 目录，不再输出到源码目录

    install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    ${PROJECT_NAME}ConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    cmake/${PROJECT_NAME}Config.cmake.in
    ${PROJECT_NAME}Config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

message(STATUS "Engineering track: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Build testing: ${BUILD_TESTING}")