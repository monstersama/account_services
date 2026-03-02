# ============ 全链路 E2E 工具 ============
find_package(yaml-cpp REQUIRED)
set(ACCT_TOOLS_YAMLCPP_TARGET yaml-cpp)
if(TARGET yaml-cpp::yaml-cpp)
    set(ACCT_TOOLS_YAMLCPP_TARGET yaml-cpp::yaml-cpp)
endif()

add_executable(full_chain_observer
    full_chain_observer.cpp
    full_chain_observer_config.cpp
    full_chain_observer_order_watch.cpp
    full_chain_observer_position_watch.cpp
    full_chain_observer_csv_sink.cpp
)

target_link_libraries(full_chain_observer PRIVATE
    acct_order_monitor
    acct_position_monitor
    ${ACCT_TOOLS_YAMLCPP_TARGET}
    rt
)

add_executable(order_submit_cli
    order_submit_cli.cpp
)

target_link_libraries(order_submit_cli PRIVATE acct_order rt)
