cmake_minimum_required(VERSION 3.16)
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)   # 允许 CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
endif()
project(ipc_firmware C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(MSVC AND NOT CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")   # /Z7，避免多进程 PDB 竞争
endif()

# 平台与产品选择（新增平台只需在 platform/ 下增加目录）
set(IPC_PLATFORM "mock" CACHE STRING "Target platform: mock | gk7205v200 | gk7205v300 | rv1106 ...")
set(IPC_PROFILE  "mock-x86" CACHE STRING "Product profile name under profiles/ (without .json)")

if(NOT EXISTS ${CMAKE_SOURCE_DIR}/platform/${IPC_PLATFORM}/CMakeLists.txt)
    message(FATAL_ERROR "Unknown IPC_PLATFORM '${IPC_PLATFORM}': platform/${IPC_PLATFORM}/CMakeLists.txt not found")
endif()
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/profiles/${IPC_PROFILE}.json)
    message(FATAL_ERROR "Unknown IPC_PROFILE '${IPC_PROFILE}': profiles/${IPC_PROFILE}.json not found")
endif()

if(MSVC)
    add_compile_options(/W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror=implicit-function-declaration)
endif()

# BUILD_TESTING 默认值按平台区分：CTest 模块的 option(BUILD_TESTING ... ON)
# 默认全平台开启，但本仓库的 docker/scripts/build-app.sh 交叉编译到 ARM
# （gk7205v200）时复用的正是这同一份顶层文件、且不会显式传
# -DBUILD_TESTING=OFF——若放任默认值，modules/common/http_server/CMakeLists.txt
# 依附 BUILD_TESTING 的 IPC_TESTING 定义就会被动带进 ARM 生产固件，
# 把测试桩函数（如 http_ws_test_conn_new）暴露进产线镜像，这正是该宏
# 拆分为按需开启要避免的事。因此在 include(CTest) 之前，只在还没有人显式
# 传值时，按平台设一个更安全的默认：mock（x86 测试构建）开，其余平台关。
# 命令行仍可用 -DBUILD_TESTING=ON/OFF 显式覆盖（NOT DEFINED 只在没有人
# 设过时生效，覆盖不了显式传参）。
if(NOT DEFINED BUILD_TESTING)
    if(IPC_PLATFORM STREQUAL "mock")
        set(BUILD_TESTING ON CACHE BOOL "构建测试树（含 ctest 用例与 IPC_TESTING 测试桩）")
    else()
        set(BUILD_TESTING OFF CACHE BOOL "构建测试树（含 ctest 用例与 IPC_TESTING 测试桩）")
    endif()
endif()
# 用 include(CTest) 而非裸 enable_testing()：后者不会创建 BUILD_TESTING
# 这个 cache 变量（它是 CTest 模块自己 option() 出来的），include(CTest)
# 内部会顺带调用 enable_testing()，两者语义因此完全保留；上面预先写好的
# cache 值会被 option() 直接沿用（cache 条目已存在时 option() 不做任何事）。
# 必须放在 add_subdirectory(modules) 之前：modules/common/http_server/
# CMakeLists.txt 求值 if(BUILD_TESTING) 时该变量必须已经存在于 cache，
# 否则等价于关闭，测试桩符号缺失导致链接失败（且这个坑不会在后续
# configure 中自愈——一旦 cache 里写过一次 BUILD_TESTING，下次 configure
# 提前可见，会掩盖干净构建时已经出现过的错）。
include(CTest)

# L1 HAL：接口 + 聚合层
add_library(ipc_hal STATIC hal/hal.c)
target_include_directories(ipc_hal PUBLIC ${CMAKE_SOURCE_DIR})

# L0 平台实现（导出 hal_platform_get）
add_subdirectory(platform/${IPC_PLATFORM})

# L2 核心服务
add_subdirectory(core)

# L3 功能模块与共享部件
add_subdirectory(modules)

# 主程序（依赖 modules 与 platform）
add_subdirectory(app)

# 门禁：core/ 与 modules/ 禁止包含 platform 头与平台宏（存在源码时启用）
file(GLOB_RECURSE IPC_GENERIC_SOURCES CONFIGURE_DEPENDS core/*.c core/*.h modules/*.c modules/*.h)
foreach(src ${IPC_GENERIC_SOURCES})
    file(READ ${src} _content)
    if(_content MATCHES "#include[ \t]+\"platform/" OR _content MATCHES "PLATFORM_|GK7205|RV1106|HI35[0-9]+")
        message(FATAL_ERROR "Layering violation in ${src}: platform-specific include or macro in generic layer")
    endif()
endforeach()

add_subdirectory(tests/hal_conformance)

# http_server_test 不碰 profile 与 HAL（纯 socket/协议层），任何平台都能跑
add_subdirectory(tests/http_server_test)

# core_test / console_test 硬编码加载 profiles/mock-x86.json（core_test/main.c:126、
# console_test/main.c:1956），在非 mock 平台上取不到 profile 后会继续执行直至段错误。
# 它们是 mock 平台专用测试，只在 mock 构建注册；非 mock 平台的通用层行为由 mock
# 构建保证。
if(IPC_PLATFORM STREQUAL "mock")
    add_subdirectory(tests/core_test)
    add_subdirectory(tests/console_test)
endif()

if(IPC_PLATFORM STREQUAL "gk7205v200")
    add_subdirectory(tests/platform_gk_test)
endif()

message(STATUS "IPC_PLATFORM=${IPC_PLATFORM}  IPC_PROFILE=${IPC_PROFILE}")
