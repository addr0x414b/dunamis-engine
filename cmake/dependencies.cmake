include(FetchContent)

# assimp
FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG v5.4.3
)

# glm
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
)

# SDL2 and Vulkan
find_package(SDL2 CONFIG REQUIRED)
find_package(Vulkan REQUIRED)

# ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9
)

FetchContent_MakeAvailable(imgui)

# Create ImGui library
add_library(imgui 
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

set_property(TARGET imgui PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(imgui 
    PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${SDL2_INCLUDE_DIRS}
        ${glm_SOURCE_DIR}
)

target_link_libraries(imgui 
    PUBLIC
        ${SDL2_LIBRARIES}
        Vulkan::Vulkan
)

FetchContent_MakeAvailable(assimp glm imgui)