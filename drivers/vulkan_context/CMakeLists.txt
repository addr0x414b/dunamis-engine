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
)

find_package(SDL2 CONFIG REQUIRED)
target_link_libraries(vulkan_context
    PRIVATE
    $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
    $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
)

find_package(Vulkan REQUIRED)
target_link_libraries(vulkan_context PRIVATE Vulkan::Vulkan)

find_package(assimp CONFIG REQUIRED)
target_link_libraries(vulkan_context PRIVATE assimp::assimp)

find_package(glm CONFIG REQUIRED)
target_link_libraries(vulkan_context PRIVATE glm::glm)

find_package(imgui CONFIG REQUIRED)
target_link_libraries(vulkan_context PRIVATE imgui::imgui)

target_link_libraries(vulkan_context PRIVATE stb_image)