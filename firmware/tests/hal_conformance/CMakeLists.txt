add_executable(hal_conformance main.c)
target_link_libraries(hal_conformance PRIVATE ipc_platform ipc_hal)
target_include_directories(hal_conformance PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME hal_conformance
         COMMAND hal_conformance ${CMAKE_SOURCE_DIR}/profiles/${IPC_PROFILE}.json
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
