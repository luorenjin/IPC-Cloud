add_library(ipc_http_server STATIC
    http_parse.c
)
target_include_directories(ipc_http_server PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ipc_http_server PUBLIC ipc_hal)
