# main.c 无条件引用 mock 平台专属符号 mock_ota_expected_digest（运行期只在
# platform_id=="mock" 分支才会真正调用，见 main.c 里 test_ota() 的注释）；
# mock 平台自带该符号的定义（platform/mock/mock_sys.c），非 mock 平台的
# ipc_platform 库里没有，故需额外链接占位实现以补齐链接期符号，运行期不会
# 被执行到，见 ota_digest_shim.c 顶部注释。
if(IPC_PLATFORM STREQUAL "mock")
    add_executable(hal_conformance main.c)
else()
    add_executable(hal_conformance main.c ota_digest_shim.c)
endif()
target_link_libraries(hal_conformance PRIVATE ipc_platform ipc_hal)
target_include_directories(hal_conformance PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME hal_conformance
         COMMAND hal_conformance ${CMAKE_SOURCE_DIR}/profiles/${IPC_PROFILE}.json
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# gk_crypto.c 的安全存储默认路径是 /etc/ipc/sec（真机路径，真机上由 rootfs
# 预建）。hal_conformance 在开发机/CI 上直接以默认路径运行会往系统真实路径
# 落盘（Windows 上是当前盘符根的 \etc\ipc\sec，非 root 的 Linux 上会
# mkdir EACCES 导致 HAL-08 crypto 断言全挂）。用 IPC_SEC_DIR 环境变量把
# ctest 跑的这次调用重定向到构建目录内，不影响真机默认行为（真机不会设置
# 这个环境变量）。只有 gk_crypto.c 读取该变量，对 mock 平台无影响。
#
# 用相对路径（不拼 CMAKE_BINARY_DIR 的绝对路径）：本仓库路径含一个不可见的
# U+200C（零宽不连字符），实测 CMake/ctest 在 Windows 上把它编码进
# ENVIRONMENT 属性的值时会被折成 "?"（Windows 路径非法字符），导致目录
# 创建失败。hal_conformance 的 WORKING_DIRECTORY 已经是 CMAKE_BINARY_DIR，
# 相对路径不受这个编码问题影响，效果等价。
set_tests_properties(hal_conformance PROPERTIES
    ENVIRONMENT "IPC_SEC_DIR=hal_conf_sec")
