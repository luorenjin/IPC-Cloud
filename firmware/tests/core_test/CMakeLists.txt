add_executable(core_test main.c)
target_link_libraries(core_test PRIVATE ipc_core ipc_platform ipc_hal)
target_include_directories(core_test PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME core_test
         COMMAND core_test
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
