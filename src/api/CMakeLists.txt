# 注意：API 版本号现在由根 CMakeLists.txt 统一管理
# ACCT_API_VERSION_* 变量已在父目录定义

# ============ 共享库 libacct_order.so (下单 API) ============
add_library(acct_order SHARED
    order_api.cpp
)

target_compile_definitions(acct_order PRIVATE ACCT_API_EXPORT)
target_link_libraries(acct_order PRIVATE acct_common acct_shm rt pthread)

set_target_properties(acct_order PROPERTIES
    VERSION ${ACCT_API_VERSION}
    SOVERSION ${ACCT_API_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# ============ 共享库 libacct_order_monitor.so (监控 API) ============
add_library(acct_order_monitor SHARED
    order_monitor_api.cpp
)

target_compile_definitions(acct_order_monitor PRIVATE ACCT_API_EXPORT)
target_link_libraries(acct_order_monitor PRIVATE rt)

set_target_properties(acct_order_monitor PROPERTIES
    VERSION ${ACCT_API_VERSION}
    SOVERSION ${ACCT_API_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# ============ 共享库 libacct_position_monitor.so (持仓监控 API) ============
add_library(acct_position_monitor SHARED
    position_monitor_api.cpp
)

target_compile_definitions(acct_position_monitor PRIVATE ACCT_API_EXPORT)
target_link_libraries(acct_position_monitor PRIVATE rt)

set_target_properties(acct_position_monitor PROPERTIES
    VERSION ${ACCT_API_VERSION}
    SOVERSION ${ACCT_API_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# ============ 安装 ============
install(TARGETS acct_order acct_order_monitor acct_position_monitor
    LIBRARY DESTINATION lib
)

install(FILES
    ${CMAKE_SOURCE_DIR}/include/api/order_api.h
    ${CMAKE_SOURCE_DIR}/include/api/order_monitor_api.h
    ${CMAKE_SOURCE_DIR}/include/api/position_monitor_api.h
    DESTINATION include/acct
)
