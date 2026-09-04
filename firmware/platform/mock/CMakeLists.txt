# mock 平台（x86），供 core/modules 与 HAL 一致性测试使用
add_library(ipc_platform STATIC
    mock_platform.c
    mock_video.c
    mock_sys.c
    mock_misc.c
)
target_include_directories(ipc_platform PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ipc_platform PUBLIC ipc_hal)
