cmake_minimum_required(VERSION 3.28)
project(DunamisEngine)

set(CMAKE_CXX_STANDARD 23)

add_executable(DunamisEngine main.cpp)

add_subdirectory(core)
add_subdirectory(servers)
add_subdirectory(drivers)
add_subdirectory(third_party)
add_subdirectory(editor)

target_link_libraries(DunamisEngine PRIVATE dunamis)

set(SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/drivers/vulkan_context/shaders")
set(SHADER_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/drivers/vulkan_context/shaders")

add_custom_command(
        TARGET DunamisEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${SHADER_SOURCE_DIR} ${SHADER_BINARY_DIR}
)


set(TEST_MODELS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/test_models/")
set(TEST_MODELS_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/test_models/")

add_custom_command(
        TARGET DunamisEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${TEST_MODELS_SOURCE_DIR} ${TEST_MODELS_BINARY_DIR}
)