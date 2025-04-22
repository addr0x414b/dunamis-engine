add_library(VulkanUtils INTERFACE)
target_sources(VulkanUtils INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/utils/vulkan_utils.h)
target_include_directories(VulkanUtils INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/utils)
target_link_libraries(VulkanUtils INTERFACE glm)

add_library(VulkanContext STATIC vulkan_context.cpp)
target_include_directories(VulkanContext PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(VulkanContext PRIVATE SDL3::SDL3 Vulkan::Vulkan spdlog VulkanUtils GameObject Scene glm)

add_library(VisualServer STATIC visual_server.cpp)
target_include_directories(VisualServer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(VisualServer PRIVATE SDL3::SDL3 VulkanContext VulkanUtils Scene assimp)
