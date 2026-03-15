# ============ 工具程序 ============
add_subdirectory(full_chain_e2e)

add_executable(market_data_quote_cli
    market_data_quote_cli.cpp
)

target_link_libraries(market_data_quote_cli PRIVATE
    acct_market_data
    acct_core_service
    acct_common
    rt
)
