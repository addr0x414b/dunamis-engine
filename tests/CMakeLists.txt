add_executable(StartupShutdownTests startup_shutdown_tests.cpp)

target_include_directories(StartupShutdownTests PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(StartupShutdownTests PRIVATE
        Dunamis
        VulkanContext
        SDL3::SDL3
        Vulkan::Vulkan
        VulkanUtils
        assimp
        glm
        spdlog
)

add_test(NAME startup_shutdown_partial_cleanup
        COMMAND StartupShutdownTests
)
set_tests_properties(startup_shutdown_partial_cleanup PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy"
)

add_test(NAME startup_failure_returns_nonzero
        COMMAND ${CMAKE_COMMAND}
                -DENGINE=$<TARGET_FILE:DunamisEngine>
                -DENGINE_WORKING_DIRECTORY=$<TARGET_FILE_DIR:DunamisEngine>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/expect_startup_failure.cmake
)
set_tests_properties(startup_failure_returns_nonzero PROPERTIES
        TIMEOUT 10
)

add_executable(VulkanPartialCleanupTests vulkan_partial_cleanup_tests.cpp)
add_dependencies(VulkanPartialCleanupTests DunamisEngine)

target_include_directories(VulkanPartialCleanupTests PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(VulkanPartialCleanupTests PRIVATE
        Dunamis
        VulkanContext
        SDL3::SDL3
        Vulkan::Vulkan
        VulkanUtils
        assimp
        glm
        spdlog
)

set(VULKAN_PARTIAL_CLEANUP_CWD
        "${CMAKE_CURRENT_BINARY_DIR}/vulkan-partial-cleanup-cwd")
file(MAKE_DIRECTORY "${VULKAN_PARTIAL_CLEANUP_CWD}")

add_test(NAME vulkan_partial_cleanup
        COMMAND VulkanPartialCleanupTests "${CMAKE_BINARY_DIR}"
)
set_tests_properties(vulkan_partial_cleanup PROPERTIES
        WORKING_DIRECTORY "${VULKAN_PARTIAL_CLEANUP_CWD}"
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION
                "has not been destroyed;leaked object|VK_IMAGE_LAYOUT_UNDEFINED"
        TIMEOUT 30
)

add_executable(SceneInvariantTests scene_invariant_tests.cpp)

target_include_directories(SceneInvariantTests PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(SceneInvariantTests PRIVATE
        Scene
        GameObject
        Level1
        Player
        SDL3::SDL3
        Vulkan::Vulkan
        VulkanUtils
        assimp
        glm
        spdlog
)

add_test(NAME scene_invariants
        COMMAND SceneInvariantTests
)

add_executable(EditorPickingTests editor_picking_tests.cpp)
target_include_directories(EditorPickingTests PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(EditorPickingTests PRIVATE
        DUNAMIS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(EditorPickingTests PRIVATE
        EditorPicking
        GameObject
        VulkanUtils
        assimp
        glm
        spdlog)
add_test(NAME editor_picking_geometry COMMAND EditorPickingTests)

add_executable(TextureLoadingTests texture_loading_tests.cpp)
target_include_directories(TextureLoadingTests PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(TextureLoadingTests PRIVATE
        GameObject
        VulkanUtils
        assimp
        glm
        spdlog
)
add_dependencies(TextureLoadingTests DunamisEngine)
add_test(NAME texture_loading_fallbacks
        COMMAND TextureLoadingTests "${CMAKE_BINARY_DIR}")
set_tests_properties(texture_loading_fallbacks PROPERTIES TIMEOUT 10)

add_executable(GameObjectOwnershipTests game_object_ownership_tests.cpp)
target_include_directories(GameObjectOwnershipTests PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(GameObjectOwnershipTests PRIVATE
        DUNAMIS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(GameObjectOwnershipTests PRIVATE
        GameObject
        VulkanUtils
        assimp
        glm
        spdlog)
add_test(NAME game_object_ownership COMMAND GameObjectOwnershipTests)

add_executable(ModelLoadingCacheTests model_loading_cache_tests.cpp)
target_include_directories(ModelLoadingCacheTests PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(ModelLoadingCacheTests PRIVATE
        DUNAMIS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(ModelLoadingCacheTests PRIVATE
        GameObject VulkanUtils assimp glm spdlog)
add_test(NAME model_loading_cache COMMAND ModelLoadingCacheTests)

add_executable(ModelLoadingBenchmark model_loading_benchmark.cpp)
target_include_directories(ModelLoadingBenchmark PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ModelLoadingBenchmark PRIVATE
        GameObject
        VulkanUtils
        assimp
        glm
        spdlog
)

add_executable(ModelLoadingCacheKeyTests model_loading_cache_key_tests.cpp)
target_include_directories(ModelLoadingCacheKeyTests PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME model_loading_cache_keys
        COMMAND ModelLoadingCacheKeyTests)

add_executable(VulkanTexturePreflightTests vulkan_texture_preflight_tests.cpp)
target_include_directories(VulkanTexturePreflightTests PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(VulkanTexturePreflightTests PRIVATE
        VulkanContext
        GameObject
        Scene
        SDL3::SDL3
        Vulkan::Vulkan
        VulkanUtils
        assimp
        glm
        spdlog
)
add_test(NAME vulkan_texture_preflight
        COMMAND VulkanTexturePreflightTests)

add_executable(RendererConfigurationTests renderer_configuration_tests.cpp)
target_include_directories(RendererConfigurationTests PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(RendererConfigurationTests PRIVATE
        VulkanUtils
        Vulkan::Vulkan
        glm
)
add_test(NAME renderer_configuration COMMAND RendererConfigurationTests)

add_executable(DirectionalShadowTests directional_shadow_tests.cpp)
target_include_directories(DirectionalShadowTests PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(DirectionalShadowTests PRIVATE
        DirectionalShadow EditorPicking Scene GameObject VulkanUtils glm)
add_test(NAME directional_shadow_math COMMAND DirectionalShadowTests)

add_executable(AmbientOcclusionTests ambient_occlusion_tests.cpp)
target_include_directories(AmbientOcclusionTests PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(AmbientOcclusionTests PRIVATE AmbientOcclusion glm)
add_test(NAME ambient_occlusion_math COMMAND AmbientOcclusionTests)
