add_library(Time STATIC
        time.cpp
)

add_library(Dunamis STATIC
        dunamis.cpp
        platform.cpp
)

target_compile_definitions(Dunamis PRIVATE
        DUNAMIS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_link_libraries(Dunamis PUBLIC nlohmann_json::nlohmann_json PRIVATE spdlog VisualServer SDL3::SDL3 VulkanUtils assimp Level1 InputManager Camera Scene Time Physics Editor glm)
