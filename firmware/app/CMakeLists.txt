# 固件主程序：全仓库唯一的非测试可执行目标
add_executable(ipc_app main.c)
# 链接顺序：GNU ld 单遍扫描静态库，谁引用谁必须排在被引用者前面。
# ipc_hal（hal.c）调用 ipc_platform 导出的 hal_platform_get()，所以 ipc_hal
# 要排在 ipc_platform 前面；MSVC 链接器不区分顺序，这条顺序约束只在
# 交叉编译（GNU ld）时才会暴露（undefined reference to `hal_platform_get'）。
target_link_libraries(ipc_app PRIVATE
    ipc_console
    ipc_http_server
    ipc_core
    ipc_hal
    ipc_platform
)
target_include_directories(ipc_app PRIVATE ${CMAKE_SOURCE_DIR})
