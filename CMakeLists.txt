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

# 头文件路径
include_directories(${CMAKE_SOURCE_DIR}/include)

# ============ 共享库 libacct_order.so ============
add_library(acct_order SHARED
    src/api/order_api.cpp
)

target_compile_definitions(acct_order PRIVATE ACCT_API_EXPORT)
target_link_libraries(acct_order PRIVATE rt pthread)

set_target_properties(acct_order PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# ============ 安装 ============
install(TARGETS acct_order
    LIBRARY DESTINATION lib
)

install(FILES
    include/api/order_api.h
    include/api/export.h
    DESTINATION include/acct
)
