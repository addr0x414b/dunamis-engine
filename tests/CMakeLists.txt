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
                "has not been destroyed;leaked object"
        TIMEOUT 30
)

add_executable(SceneInvariantTests scene_invariant_tests.cpp)

target_include_directories(SceneInvariantTests PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(SceneInvariantTests PRIVATE
        Scene
        GameObject
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
