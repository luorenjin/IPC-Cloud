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

# L1 HAL：接口 + 聚合层
add_library(ipc_hal STATIC hal/hal.c)
target_include_directories(ipc_hal PUBLIC ${CMAKE_SOURCE_DIR})

# L0 平台实现（导出 hal_platform_get）
add_subdirectory(platform/${IPC_PLATFORM})

# L2 核心服务
add_subdirectory(core)

# L3 功能模块与共享部件
add_subdirectory(modules)

# 门禁：core/ 与 modules/ 禁止包含 platform 头与平台宏（存在源码时启用）
file(GLOB_RECURSE IPC_GENERIC_SOURCES CONFIGURE_DEPENDS core/*.c core/*.h modules/*.c modules/*.h)
foreach(src ${IPC_GENERIC_SOURCES})
    file(READ ${src} _content)
    if(_content MATCHES "#include[ \t]+\"platform/" OR _content MATCHES "PLATFORM_|GK7205|RV1106|HI35[0-9]+")
        message(FATAL_ERROR "Layering violation in ${src}: platform-specific include or macro in generic layer")
    endif()
endforeach()

enable_testing()
add_subdirectory(tests/hal_conformance)
add_subdirectory(tests/core_test)
add_subdirectory(tests/http_server_test)
add_subdirectory(tests/console_test)

message(STATUS "IPC_PLATFORM=${IPC_PLATFORM}  IPC_PROFILE=${IPC_PROFILE}")
