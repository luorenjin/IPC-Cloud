# 仅在目标平台为 gk7205v200 时构建（解析逻辑本身与架构无关，可在 x86 上跑）
add_executable(platform_gk_test main.c)
target_link_libraries(platform_gk_test PRIVATE ipc_platform ipc_hal)
target_include_directories(platform_gk_test PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME platform_gk_test
         COMMAND platform_gk_test
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
