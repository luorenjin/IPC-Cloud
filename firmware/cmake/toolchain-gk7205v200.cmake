# GK7205V200 交叉编译工具链（GOKE SDK 自带 arm-gcc6.3-linux-uclibceabi）
#
# 静态链接：板子 rootfs 的 uClibc 版本与本工具链未必一致，动态链接有
# 运行时找不到库的风险。已实测 -static 下 epoll/pthread/socket 均可用。

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# 由 -DGK_TOOLCHAIN_DIR 传入，默认为 docker 内 SDK 卷的挂载位置
if(NOT GK_TOOLCHAIN_DIR)
    set(GK_TOOLCHAIN_DIR "/sdk/GKIPCLinuxV100R001C00SPC030/tools/toolchains/arm-gcc6.3-linux-uclibceabi/host_bin")
endif()

set(CMAKE_C_COMPILER   ${GK_TOOLCHAIN_DIR}/arm-linux-uclibceabi-gcc)
set(CMAKE_CXX_COMPILER ${GK_TOOLCHAIN_DIR}/arm-linux-uclibceabi-g++)
set(CMAKE_AR           ${GK_TOOLCHAIN_DIR}/arm-linux-uclibceabi-ar CACHE FILEPATH "")
set(CMAKE_RANLIB       ${GK_TOOLCHAIN_DIR}/arm-linux-uclibceabi-ranlib CACHE FILEPATH "")
set(CMAKE_STRIP        ${GK_TOOLCHAIN_DIR}/arm-linux-uclibceabi-strip CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

# 交叉编译时 CMake 的编译器探测默认会尝试运行产物，ARM 产物在 x86 上跑不了
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
