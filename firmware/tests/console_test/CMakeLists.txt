add_executable(console_test main.c)
target_link_libraries(console_test PRIVATE ipc_console ipc_http_server ipc_core ipc_platform ipc_hal)
target_include_directories(console_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(console_test PRIVATE IPC_TESTING)

add_test(NAME console_test
         COMMAND console_test
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
