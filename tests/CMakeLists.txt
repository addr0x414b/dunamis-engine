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
