add_library(Dunamis STATIC dunamis.cpp)

target_include_directories(Dunamis PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(Dunamis PRIVATE spdlog VisualServer SDL3::SDL3 VulkanUtils assimp Level1 InputManager)