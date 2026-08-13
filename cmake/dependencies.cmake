include(FetchContent)

# Dear ImGui is directly vendored under third_party/imgui.
# Repository: https://github.com/ocornut/imgui
# Branch: docking
# Pinned commit: 94c05ba40b2f8a52cefb81f022d6c409ba979163
# Version: 1.93.0 WIP

FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG v6.0.5
)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "Build Assimp's bundled zlib/minizip" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "Do not treat Assimp warnings as errors" FORCE)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.2
)

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
set(JSON_BuildTests OFF CACHE BOOL "Do not build nlohmann/json tests" FORCE)

FetchContent_MakeAvailable(spdlog glm assimp nlohmann_json)

find_package(SDL3 REQUIRED CONFIG)
find_package(Vulkan REQUIRED)
