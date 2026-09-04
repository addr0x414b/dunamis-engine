add_library(GameObject STATIC game_object.cpp model_renderable.cpp)
target_include_directories(GameObject PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(GameObject PUBLIC spdlog glm assimp PRIVATE Threads::Threads)

add_library(Camera STATIC camera.cpp)
target_include_directories(Camera PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Camera PRIVATE spdlog glm GameObject)

add_library(PointLight STATIC scene.cpp)
target_include_directories(PointLight PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(PointLight PRIVATE spdlog glm GameObject)

add_library(Scene STATIC scene.cpp scene_manager.cpp directional_light.cpp
        type_registry.cpp scene_serializer.cpp persistent_id.cpp group.cpp)
target_include_directories(Scene PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Scene PUBLIC nlohmann_json::nlohmann_json PRIVATE spdlog glm GameObject Camera PointLight InputManager)
