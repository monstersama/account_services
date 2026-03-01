# src/CMakeLists.txt

find_package(yaml-cpp REQUIRED)
find_package(SQLite3 QUIET)
set(ACCT_YAMLCPP_TARGET yaml-cpp)
if(TARGET yaml-cpp::yaml-cpp)
    set(ACCT_YAMLCPP_TARGET yaml-cpp::yaml-cpp)
endif()

# ============ 核心库 ============

# common 库 (spinlock, time_utils)
add_library(acct_common STATIC
    common/spinlock.cpp
    common/time_utils.cpp
    common/error.cpp
    common/log.cpp
)
target_include_directories(acct_common PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

# shm 库 (shm_manager)
add_library(acct_shm STATIC
    shm/shm_manager.cpp
)
target_include_directories(acct_shm PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)
target_link_libraries(acct_shm PRIVATE acct_common rt)

# portfolio 库 (position/account/trade/entrust)
add_library(acct_portfolio STATIC
    portfolio/position_manager.cpp
    portfolio/position_loader.cpp
    portfolio/account_info.cpp
    portfolio/trade_record.cpp
    portfolio/entrust_record.cpp
)
target_include_directories(acct_portfolio PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)
if(TARGET SQLite::SQLite3)
    target_link_libraries(acct_portfolio PUBLIC acct_common SQLite::SQLite3)
elseif(SQLite3_FOUND)
    target_include_directories(acct_portfolio PUBLIC ${SQLite3_INCLUDE_DIRS})
    target_link_libraries(acct_portfolio PUBLIC acct_common ${SQLite3_LIBRARIES})
else()
    find_library(ACCT_SQLITE3_RUNTIME_LIB NAMES sqlite3)
    if(NOT ACCT_SQLITE3_RUNTIME_LIB)
        find_library(
            ACCT_SQLITE3_RUNTIME_LIB
            NAMES sqlite3.so.0 libsqlite3.so.0
            PATHS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu
        )
    endif()
    if(NOT ACCT_SQLITE3_RUNTIME_LIB)
        message(FATAL_ERROR "SQLite3 not found: install sqlite3 runtime/dev packages or provide sqlite3 library path.")
    endif()
    target_link_libraries(acct_portfolio PUBLIC acct_common ${ACCT_SQLITE3_RUNTIME_LIB})
endif()

# order 核心库 (order_book, order_router, order_splitter)
add_library(acct_order_core STATIC
    order/order_book.cpp
    order/order_router.cpp
    order/order_splitter.cpp
)
target_include_directories(acct_order_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)
target_link_libraries(acct_order_core PUBLIC acct_common)

# risk 库 (risk_checker, risk_manager)
add_library(acct_risk STATIC
    risk/risk_checker.cpp
    risk/risk_manager.cpp
)
target_include_directories(acct_risk PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)
target_link_libraries(acct_risk PUBLIC acct_common acct_portfolio)

# core 事件循环库
add_library(acct_core_loop STATIC
    core/event_loop.cpp
)
target_include_directories(acct_core_loop PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)
target_link_libraries(acct_core_loop PUBLIC acct_common acct_order_core acct_risk acct_portfolio)

# core 服务库 (config_manager, account_service)
add_library(acct_core_service STATIC
    core/config_manager.cpp
    core/account_service.cpp
)
target_include_directories(acct_core_service PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)
target_link_libraries(acct_core_service PUBLIC
    acct_common
    acct_shm
    acct_portfolio
    acct_order_core
    acct_risk
    acct_core_loop
    ${ACCT_YAMLCPP_TARGET}
)

# broker API 共享库
add_library(acct_broker_api SHARED
    broker_api/broker_api.cpp
)
target_compile_definitions(acct_broker_api PRIVATE ACCT_BROKER_API_EXPORT)
target_include_directories(acct_broker_api PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/generated
)
set_target_properties(acct_broker_api PROPERTIES
    VERSION ${ACCT_API_VERSION}
    SOVERSION ${ACCT_API_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

install(TARGETS acct_broker_api
    LIBRARY DESTINATION lib
)

install(FILES
    ${CMAKE_SOURCE_DIR}/include/broker_api/broker_api.hpp
    ${CMAKE_SOURCE_DIR}/include/broker_api/export.h
    DESTINATION include/broker_api
)

# 主程序
add_executable(acct_service_main
    main.cpp
)
target_link_libraries(acct_service_main PRIVATE acct_core_service)
