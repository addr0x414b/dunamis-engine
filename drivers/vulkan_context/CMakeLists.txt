add_library(vulkan_context "")

target_sources(vulkan_context
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/vulkan_context.cpp
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/vulkan_context.h
)

target_include_directories(vulkan_context
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}
    PRIVATE
        ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(vulkan_context 
    PUBLIC
        Vulkan::Vulkan
        ${SDL2_LIBRARIES}
        imgui
        stb_image
        scene
)