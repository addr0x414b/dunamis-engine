add_library(Physics STATIC
        physics_server.cpp
        physics_mesh_builder.cpp)

target_include_directories(Physics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Physics PUBLIC Scene Time PRIVATE Jolt spdlog glm Threads::Threads)
