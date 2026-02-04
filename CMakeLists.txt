cmake_minimum_required(VERSION 3.16)
project(account_services VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 编译选项
add_compile_options(-Wall -Wextra -Wpedantic)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g3")

# ============ 版本号定义 ============

# API 版本 (libacct_order.so) - 必须在 configure_file 之前定义
set(ACCT_API_VERSION_MAJOR 1)
set(ACCT_API_VERSION_MINOR 0)
set(ACCT_API_VERSION_PATCH 3)
set(ACCT_API_VERSION "${ACCT_API_VERSION_MAJOR}.${ACCT_API_VERSION_MINOR}.${ACCT_API_VERSION_PATCH}")

# 生成版本头文件
configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/version.h.in
    ${CMAKE_BINARY_DIR}/generated/version.h
    @ONLY
)

# 头文件路径
# include/ - 对外公开API头文件
# src/     - 内部实现头文件
# generated/ - CMake生成的头文件
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

# ============ 子模块 ============
add_subdirectory(src)
add_subdirectory(src/api)
add_subdirectory(test)
add_subdirectory(examples)
