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
