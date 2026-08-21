add_library(Physics STATIC
        physics_server.cpp
        jolt_shape_builder.cpp
        physics_mesh_builder.cpp
        physics_shape_cache.cpp)

target_include_directories(Physics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(Physics PRIVATE
        DUNAMIS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(Physics PUBLIC Scene Time PRIVATE Jolt spdlog glm Threads::Threads)
