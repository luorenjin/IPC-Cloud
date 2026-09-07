add_library(ipc_console STATIC
    console.c
    console_auth.c
    console_api.c
)
target_include_directories(ipc_console PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ipc_console PUBLIC ipc_hal ipc_core ipc_http_server)
