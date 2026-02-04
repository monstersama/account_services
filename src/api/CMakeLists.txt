# API 版本 (libacct_order.so) - 独立控制
set(ACCT_API_VERSION_MAJOR 1)
set(ACCT_API_VERSION_MINOR 0)
set(ACCT_API_VERSION_PATCH 3)
set(ACCT_API_VERSION "${ACCT_API_VERSION_MAJOR}.${ACCT_API_VERSION_MINOR}.${ACCT_API_VERSION_PATCH}")

# ============ 共享库 libacct_order.so ============
add_library(acct_order SHARED
    order_api.cpp
)

target_compile_definitions(acct_order PRIVATE ACCT_API_EXPORT)
target_link_libraries(acct_order PRIVATE rt pthread)

set_target_properties(acct_order PROPERTIES
    VERSION ${ACCT_API_VERSION}
    SOVERSION ${ACCT_API_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# ============ 安装 ============
install(TARGETS acct_order
    LIBRARY DESTINATION lib
)

install(FILES
    ${CMAKE_SOURCE_DIR}/include/api/order_api.h
    ${CMAKE_SOURCE_DIR}/include/api/export.h
    DESTINATION include/acct
)
