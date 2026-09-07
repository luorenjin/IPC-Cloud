add_library(ipc_http_server STATIC
    http_parse.c
    http_server.c
)
target_include_directories(ipc_http_server PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ipc_http_server PUBLIC ipc_hal ipc_core)

# Windows 下 socket API（WSAStartup/socket/bind/...）由 ws2_32 提供；PUBLIC 是因为
# http_server_test 需要在测试里发起真实 client 端 socket 连接，借由链接
# ipc_http_server 透传拿到 ws2_32，无需在测试的 CMakeLists 里重复声明。
if (WIN32)
    target_link_libraries(ipc_http_server PUBLIC ws2_32)
endif()
