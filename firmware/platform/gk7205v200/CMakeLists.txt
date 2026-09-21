# GK7205V200 平台实现（ARM Linux 4.9）
# 本期只实现控制台用到的 sys/net/storage/crypto，video/gpio 为 HAL_ENOTSUP 桩
add_library(ipc_platform STATIC
    gk_procfs.c
    gk_sys.c
    gk_net.c
    gk_storage.c
    gk_crypto.c
)
target_include_directories(ipc_platform PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ipc_platform PUBLIC ipc_hal)
