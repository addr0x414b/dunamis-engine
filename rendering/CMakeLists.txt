add_library(VulkanUtils INTERFACE)
target_sources(VulkanUtils INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/utils/vulkan_utils.h)
target_include_directories(VulkanUtils INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/utils)
target_link_libraries(VulkanUtils INTERFACE glm)

add_library(EditorPicking STATIC editor_picking.cpp)
target_include_directories(EditorPicking PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(EditorPicking PRIVATE VulkanUtils GameObject Camera glm)

add_library(DirectionalShadow STATIC directional_shadow.cpp)
target_include_directories(DirectionalShadow PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(DirectionalShadow PUBLIC Scene GameObject glm spdlog)

add_library(AmbientOcclusion STATIC ambient_occlusion.cpp)
target_include_directories(AmbientOcclusion PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(AmbientOcclusion PUBLIC glm)

set(IMGUI_DIR ${PROJECT_SOURCE_DIR}/third_party/imgui)

add_library(VulkanContext STATIC
        vulkan_context.cpp
        imgui_layer.cpp
        ${PROJECT_SOURCE_DIR}/third_party/imguizmo/ImGuizmo.cpp
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
        ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(VulkanContext PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(VulkanContext SYSTEM PRIVATE
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${PROJECT_SOURCE_DIR}/third_party/imguizmo)
target_link_libraries(VulkanContext PRIVATE SDL3::SDL3 Vulkan::Vulkan spdlog VulkanUtils EditorPicking DirectionalShadow AmbientOcclusion GameObject Scene glm)

add_library(VisualServer STATIC visual_server.cpp)
target_include_directories(VisualServer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(VisualServer PRIVATE SDL3::SDL3 VulkanContext VulkanUtils Scene assimp spdlog)
