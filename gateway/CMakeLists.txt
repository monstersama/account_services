add_library(acct_gateway_core STATIC
    src/gateway_config.cpp
    src/order_mapper.cpp
    src/response_mapper.cpp
    src/sim_broker_adapter.cpp
    src/gateway_loop.cpp
)

target_include_directories(acct_gateway_core PUBLIC
    ${CMAKE_SOURCE_DIR}/gateway/src
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/generated
)

target_link_libraries(acct_gateway_core PUBLIC
    acct_common
    acct_shm
    acct_broker_api
)

add_executable(acct_broker_gateway_main
    src/main.cpp
)

target_link_libraries(acct_broker_gateway_main PRIVATE acct_gateway_core)

