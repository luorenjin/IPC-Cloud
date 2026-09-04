# L2 核心服务
add_library(ipc_core STATIC
    src/os.c
    src/json.c
    src/log.c
    src/profile.c
    src/event_bus.c
    src/config.c
    src/frame_bus.c
    src/module_loader.c
)
target_include_directories(ipc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ipc_core PUBLIC ipc_hal)

if(UNIX)
    target_link_libraries(ipc_core PUBLIC pthread)
endif()
