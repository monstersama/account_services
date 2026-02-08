# src/CMakeLists.txt

# ============ 核心库 ============

# common 库 (spinlock, time_utils)
add_library(acct_common STATIC
    common/spinlock.cpp
    common/time_utils.cpp
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
target_link_libraries(acct_risk PUBLIC acct_common)

# core 事件循环库
add_library(acct_core_loop STATIC
    core/event_loop.cpp
)
target_include_directories(acct_core_loop PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)
target_link_libraries(acct_core_loop PUBLIC acct_common acct_order_core acct_risk)

# ============ 后续阶段将添加更多库 ============
# - portfolio (position_manager, account_info, trade_record, entrust_record)
# - order (order_book, order_router, order_splitter)
# - risk (risk_checker, risk_manager)
# - core (config_manager, event_loop, account_service, main)
