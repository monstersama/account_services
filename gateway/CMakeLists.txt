find_package(yaml-cpp REQUIRED)
set(ACCT_GATEWAY_YAMLCPP_TARGET yaml-cpp)
if(TARGET yaml-cpp::yaml-cpp)
    set(ACCT_GATEWAY_YAMLCPP_TARGET yaml-cpp::yaml-cpp)
endif()

add_library(acct_gateway_core STATIC
    src/adapter_loader.cpp
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
    ${ACCT_GATEWAY_YAMLCPP_TARGET}
    dl
)

add_executable(acct_broker_gateway_main
    src/main.cpp
)

target_link_libraries(acct_broker_gateway_main PRIVATE acct_gateway_core)

add_library(acct_gateway_sim_plugin SHARED
    src/sim_broker_plugin.cpp
)

target_link_libraries(acct_gateway_sim_plugin PRIVATE acct_gateway_core)
