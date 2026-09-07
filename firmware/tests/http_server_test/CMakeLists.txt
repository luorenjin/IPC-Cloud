add_executable(http_server_test main.c)
target_link_libraries(http_server_test PRIVATE ipc_http_server ipc_hal)
target_include_directories(http_server_test PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME http_server_test
         COMMAND http_server_test
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
