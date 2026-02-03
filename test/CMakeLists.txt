# ============ 测试可执行文件 ============
add_executable(test_order_api
    test_order_api.cpp
)

target_link_libraries(test_order_api PRIVATE acct_order)

# 注册到 CTest
enable_testing()
add_test(NAME test_order_api COMMAND test_order_api)
