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

# Jolt's standalone defaults configure broad tooling and may modify compiler
# flags in its own directory. Keep this embedded rigid-body backend narrow and
# do not let it select project-wide optimization policy.
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "Keep Dunamis compiler flags" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "Disable Jolt LTO" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "Do not enable Jolt warnings as errors" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "Do not add Jolt install targets" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE ON CACHE BOOL "Enable Jolt debug renderer" FORCE)
set(DEBUG_RENDERER_IN_DISTRIBUTION ON CACHE BOOL "Enable Jolt debug renderer" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "Disable Jolt profiler" FORCE)
set(PROFILER_IN_DISTRIBUTION OFF CACHE BOOL "Disable Jolt profiler" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "Disable Jolt DX12 compute" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "Disable Jolt Vulkan compute" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "Disable Jolt Metal compute" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "Disable Jolt CPU compute" FORCE)

FetchContent_Declare(
    JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.6.0
    SOURCE_SUBDIR Build
)

FetchContent_MakeAvailable(spdlog glm assimp nlohmann_json JoltPhysics)

find_package(SDL3 REQUIRED CONFIG)
find_package(Vulkan REQUIRED)
