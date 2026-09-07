add_library(ipc_console STATIC
    console.c
    console_auth.c
    console_api.c
)
target_include_directories(ipc_console PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ipc_console PUBLIC ipc_hal ipc_core ipc_http_server)
# 能力清单里 h265/playback 两项的编译期开关（console_caps_json 用）：本仓库
# 目前只有这一套固件变体，两项恒为开启；真正的启用与否仍由运行期的 profile
# 编解码集合 / recorder 模块是否存在决定（规则 R3），这两个宏只表示"本次
# 编译是否包含相关代码路径"。低端 SKU 需要裁掉 H265 编码器或 recorder 以
# 节省 flash 时，再改成 CMake 选项按平台/profile 关闭。
target_compile_definitions(ipc_console PUBLIC IPC_CONSOLE_H265 IPC_CONSOLE_PLAYBACK)
