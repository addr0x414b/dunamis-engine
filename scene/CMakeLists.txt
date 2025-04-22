add_library(GameObject STATIC game_object.cpp)
target_include_directories(GameObject PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(GameObject PUBLIC spdlog VulkanUtils glm assimp)

add_library(Camera STATIC camera.cpp)
target_include_directories(Camera PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Camera PRIVATE spdlog glm GameObject)

add_library(Scene STATIC scene.cpp)
target_include_directories(Scene PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Scene PRIVATE spdlog glm GameObject Camera)