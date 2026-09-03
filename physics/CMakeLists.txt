add_library(Physics STATIC
        physics_server.cpp
        physics_transforms.cpp
        collision_shapes.cpp
        physics_shape_cache.cpp)

target_include_directories(Physics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(Physics PRIVATE
        DUNAMIS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(Physics PUBLIC Scene Time PRIVATE Jolt spdlog glm Threads::Threads)
