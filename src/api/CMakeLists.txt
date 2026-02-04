# 注意：API 版本号现在由根 CMakeLists.txt 统一管理
# ACCT_API_VERSION_* 变量已在父目录定义

# ============ 共享库 libacct_order.so ============
add_library(acct_order SHARED
    order_api_cpp.cpp
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
    ${CMAKE_SOURCE_DIR}/include/api/order_api.hpp
    DESTINATION include/acct
)
