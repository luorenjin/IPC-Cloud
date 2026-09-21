# 固件主程序：全仓库唯一的非测试可执行目标
add_executable(ipc_app main.c)
target_link_libraries(ipc_app PRIVATE
    ipc_console
    ipc_http_server
    ipc_core
    ipc_platform
    ipc_hal
)
target_include_directories(ipc_app PRIVATE ${CMAKE_SOURCE_DIR})
