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

# ============ 后续阶段将添加更多库 ============
# - portfolio (position_manager, account_info, trade_record, entrust_record)
# - order (order_book, order_router, order_splitter)
# - risk (risk_checker, risk_manager)
# - core (config_manager, event_loop, account_service, main)
