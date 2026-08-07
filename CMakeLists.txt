cmake_minimum_required(VERSION 4.0)
project(DunamisEngine)

set(CMAKE_CXX_STANDARD 17)

find_package(Threads REQUIRED)

# GLM headers are included by public engine headers, so these definitions must
# be present for every first-party translation unit before it parses GLM.
add_compile_definitions(
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_ENABLE_EXPERIMENTAL
)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/dependencies.cmake)

add_subdirectory(core)
add_subdirectory(rendering)
add_subdirectory(scene)
add_subdirectory(game)
add_subdirectory(third_party)
add_subdirectory(input)

add_executable(DunamisEngine main.cpp)

target_link_libraries(DunamisEngine PRIVATE Dunamis VulkanUtils assimp spdlog)

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

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
