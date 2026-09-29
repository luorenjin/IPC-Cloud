# GK7205V200 平台实现（ARM Linux 4.9）
#
# video 的真实实现是 gk_video.c —— 它依赖 GOKE MPP SDK（厂商闭源二进制，
# 不入库），因此是否编译由构建时能否找到 SDK 决定：
#   * 交叉编译到板子：build-app.sh 传 -DIPC_MPP_DIR=<sdk>/source/gmp，
#     此时定义 IPC_HAVE_MPP 并链接 MPP 静态库、编译 gk_video.c；
#   * 本机（MSVC/x86）或任何拿不到 SDK 的构建：不传该变量，video 走
#     gk_stub.c 里的 HAL_ENOTSUP 桩，控制台其余功能与纯逻辑测试照常可跑。
# gpio 始终是桩（引脚映射超出本里程碑）。
#
# MPP 库为什么用 --start-group 包起来：lib_nolog/static 下有几十个静态库
# （GK_API_* 主体 / ISP / AE / AWB / 各 sensor 驱动 …），彼此循环引用，
# 而 GNU ld 单遍扫描静态库、按命令行顺序解析符号——不包 group 就必须人工
# 排出拓扑序（且随 SDK 版本变动而失效）。MSVC 不参与这条路径（无 MPP）。

set(GK_SOURCES
    gk_platform.c
    gk_procfs.c
    gk_sys.c
    gk_net.c
    gk_storage.c
    gk_crypto.c
    gk_gpio.c
    gk_stub.c
)

set(IPC_MPP_DIR "" CACHE PATH "GOKE MPP SDK 目录（<sdk>/source/gmp）；留空 = video 用桩实现")

set(GK_MPP_LINK_LIBS "")
set(GK_HAVE_MPP FALSE)

if(IPC_MPP_DIR AND EXISTS "${IPC_MPP_DIR}/include/gk_api_vi.h")
    set(GK_MPP_LIB_SUBDIR "lib_nolog/static")
    if(NOT EXISTS "${IPC_MPP_DIR}/${GK_MPP_LIB_SUBDIR}/libgk_api.a")
        # 优先无日志版（体积小、无 ioctl 打点开销）；缺了才退回带日志版
        set(GK_MPP_LIB_SUBDIR "lib_log/static")
    endif()
    file(GLOB GK_MPP_ARCHIVES "${IPC_MPP_DIR}/${GK_MPP_LIB_SUBDIR}/*.a")
    if(NOT GK_MPP_ARCHIVES)
        message(FATAL_ERROR "IPC_MPP_DIR=${IPC_MPP_DIR} 下没找到 ${GK_MPP_LIB_SUBDIR}/*.a（SDK 卷不完整？）")
    endif()

    list(LENGTH GK_MPP_ARCHIVES GK_MPP_N)
    set(GK_HAVE_MPP TRUE)
    # gk_osd.c 用 RGN 做叠加，同样依赖 MPP SDK，所以只在启用 MPP 时参与编译
    list(APPEND GK_SOURCES gk_video.c gk_osd.c)
    # pthread/m 供 ISP_Run 线程与 ISP 数学运算；mpi 内部还用到 dl/rt
    set(GK_MPP_LINK_LIBS -Wl,--start-group ${GK_MPP_ARCHIVES} -Wl,--end-group pthread m dl rt)
    message(STATUS "gk7205v200: MPP 已启用（${IPC_MPP_DIR}/${GK_MPP_LIB_SUBDIR}，${GK_MPP_N} 个静态库）")
elseif(IPC_MPP_DIR)
    message(FATAL_ERROR "IPC_MPP_DIR=${IPC_MPP_DIR} 下找不到 include/gk_api_vi.h")
else()
    message(STATUS "gk7205v200: 未指定 IPC_MPP_DIR，video 使用 ENOTSUP 桩")
endif()

add_library(ipc_platform STATIC ${GK_SOURCES})
target_include_directories(ipc_platform PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ipc_platform PUBLIC ipc_hal)

if(GK_HAVE_MPP)
    # IPC_HAVE_MPP 让 gk_stub.c 里的 video 桩让位给 gk_video.c
    target_compile_definitions(ipc_platform PUBLIC IPC_HAVE_MPP=1)
    target_include_directories(ipc_platform PRIVATE "${IPC_MPP_DIR}/include")
    target_link_libraries(ipc_platform PUBLIC ${GK_MPP_LINK_LIBS})

    # 板端出图自检程序：独立可执行，不依赖 HAL/控制台，用来单独验证
    # MIPI→VI→ISP→VPSS→VENC 与 GC2053 sensor 通路（详见源码注释）。
    # 只在有 MPP 时构建（本质是交叉编译到板子的调试工具）。
    add_executable(mpp_selftest mpp_selftest.c)
    target_include_directories(mpp_selftest PRIVATE "${IPC_MPP_DIR}/include")
    target_link_libraries(mpp_selftest PRIVATE ${GK_MPP_LINK_LIBS})
endif()
