cmake_minimum_required(VERSION 4.0)
project(DunamisEngine)

set(CMAKE_CXX_STANDARD 17)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/dependencies.cmake)

add_subdirectory(core)
add_subdirectory(rendering)
add_subdirectory(scene)
add_subdirectory(game)
add_subdirectory(third_party)
add_subdirectory(input)

add_executable(DunamisEngine main.cpp)

target_link_libraries(DunamisEngine PRIVATE Dunamis VulkanUtils assimp)

set(SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rendering/shaders")
set(SHADER_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/rendering/shaders")

add_custom_command(
        TARGET DunamisEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${SHADER_SOURCE_DIR} ${SHADER_BINARY_DIR}
)

set(MODELS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/game/assets/")
set(MODELS_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/game/assets/")

add_custom_command(
        TARGET DunamisEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${MODELS_SOURCE_DIR} ${MODELS_BINARY_DIR}
)

set(TEX_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rendering/default_textures")
set(TEX_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/rendering/default_textures")

add_custom_command(
        TARGET DunamisEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${TEX_SOURCE_DIR} ${TEX_BINARY_DIR}
)