# ============ 测试可执行文件 ============
add_executable(test_order_api
    test_order_api.cpp
)

target_link_libraries(test_order_api PRIVATE acct_order)

add_executable(test_order_monitor_api
    test_order_monitor_api.cpp
)

target_link_libraries(test_order_monitor_api PRIVATE acct_order acct_order_monitor)

# ============ shm_manager 测试 ============
add_executable(test_shm_manager
    test_shm_manager.cpp
)

target_link_libraries(test_shm_manager PRIVATE acct_shm)

add_executable(test_error_registry
    test_error_registry.cpp
)

target_link_libraries(test_error_registry PRIVATE acct_common)

add_executable(test_logger_async
    test_logger_async.cpp
)

target_link_libraries(test_logger_async PRIVATE acct_common acct_core_service)

add_executable(test_termination_policy
    test_termination_policy.cpp
)

target_link_libraries(test_termination_policy PRIVATE acct_common)

# ============ position_manager 测试 ============
add_executable(test_position_manager
    test_position_manager.cpp
)

target_link_libraries(test_position_manager PRIVATE acct_portfolio)

# ============ order_book 拆单追踪测试 ============
add_executable(test_order_book_split_tracking
    test_order_book_split_tracking.cpp
)

target_link_libraries(test_order_book_split_tracking PRIVATE acct_order_core)

# ============ order_router 拆单撤单测试 ============
add_executable(test_order_router_split_cancel
    test_order_router_split_cancel.cpp
)

target_link_libraries(test_order_router_split_cancel PRIVATE acct_order_core)

# ============ risk_manager 测试 ==========
add_executable(test_risk_manager
    test_risk_manager.cpp
)

target_link_libraries(test_risk_manager PRIVATE acct_risk)

# ============ event_loop 测试 ==========
add_executable(test_event_loop
    test_event_loop.cpp
)

target_link_libraries(test_event_loop PRIVATE acct_core_loop)

# ============ config_manager 测试 ==========
add_executable(test_config_manager
    test_config_manager.cpp
)

target_link_libraries(test_config_manager PRIVATE acct_core_service)

# ============ account_service 测试 ==========
add_executable(test_account_service
    test_account_service.cpp
)

target_link_libraries(test_account_service PRIVATE acct_core_service)

# ============ gateway mapper 测试 ==========
add_executable(test_gateway_mapper
    test_gateway_mapper.cpp
)

target_link_libraries(test_gateway_mapper PRIVATE acct_gateway_core)

# ============ gateway loop 测试 ==========
add_executable(test_gateway_loop
    test_gateway_loop.cpp
)

target_link_libraries(test_gateway_loop PRIVATE acct_gateway_core)

# ============ gateway plugin 模式端到端测试 ==========
add_executable(test_gateway_plugin_mode
    test_gateway_plugin_mode.cpp
)

target_link_libraries(test_gateway_plugin_mode PRIVATE acct_gateway_core)
target_compile_definitions(test_gateway_plugin_mode PRIVATE
    TEST_ADAPTER_PLUGIN_PATH="$<TARGET_FILE:acct_gateway_sim_plugin>"
)
add_dependencies(test_gateway_plugin_mode acct_gateway_sim_plugin)

# ============ adapter_loader 插件加载测试 ==========
add_executable(test_adapter_loader
    test_adapter_loader.cpp
)

target_link_libraries(test_adapter_loader PRIVATE acct_gateway_core)
target_compile_definitions(test_adapter_loader PRIVATE
    TEST_ADAPTER_PLUGIN_PATH="$<TARGET_FILE:acct_gateway_sim_plugin>"
)
add_dependencies(test_adapter_loader acct_gateway_sim_plugin)

# 注册到 CTest
enable_testing()
add_test(NAME test_order_api COMMAND test_order_api)
add_test(NAME test_order_monitor_api COMMAND test_order_monitor_api)
add_test(NAME test_shm_manager COMMAND test_shm_manager)
add_test(NAME test_error_registry COMMAND test_error_registry)
add_test(NAME test_logger_async COMMAND test_logger_async)
add_test(NAME test_termination_policy COMMAND test_termination_policy)
add_test(NAME test_position_manager COMMAND test_position_manager)
add_test(NAME test_order_book_split_tracking COMMAND test_order_book_split_tracking)
add_test(NAME test_order_router_split_cancel COMMAND test_order_router_split_cancel)
add_test(NAME test_risk_manager COMMAND test_risk_manager)
add_test(NAME test_event_loop COMMAND test_event_loop)
add_test(NAME test_config_manager COMMAND test_config_manager)
add_test(NAME test_account_service COMMAND test_account_service)
add_test(NAME test_gateway_mapper COMMAND test_gateway_mapper)
add_test(NAME test_gateway_loop COMMAND test_gateway_loop)
add_test(NAME test_gateway_plugin_mode COMMAND test_gateway_plugin_mode)
add_test(NAME test_adapter_loader COMMAND test_adapter_loader)
