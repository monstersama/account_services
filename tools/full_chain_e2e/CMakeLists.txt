# ============ 全链路 E2E 工具 ============
add_executable(full_chain_observer
    full_chain_observer.cpp
    full_chain_observer_order_watch.cpp
    full_chain_observer_position_watch.cpp
    full_chain_observer_csv_sink.cpp
)

target_link_libraries(full_chain_observer PRIVATE acct_order_monitor acct_position_monitor rt)

add_executable(order_submit_cli
    order_submit_cli.cpp
)

target_link_libraries(order_submit_cli PRIVATE acct_order rt)
