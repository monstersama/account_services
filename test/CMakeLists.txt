# ============ 测试可执行文件 ============
add_executable(test_order_api
    test_order_api.cpp
)

target_link_libraries(test_order_api PRIVATE acct_order)

# ============ shm_manager 测试 ============
add_executable(test_shm_manager
    test_shm_manager.cpp
)

target_link_libraries(test_shm_manager PRIVATE acct_shm)

# ============ position_manager 测试 ============
add_executable(test_position_manager
    test_position_manager.cpp
    ../src/portfolio/position_manager.cpp
)

target_link_libraries(test_position_manager PRIVATE acct_common)

# 注册到 CTest
enable_testing()
# add_test(NAME test_order_api COMMAND test_order_api)
# add_test(NAME test_shm_manager COMMAND test_shm_manager)
# add_test(NAME test_position_manager COMMAND test_position_manager)
