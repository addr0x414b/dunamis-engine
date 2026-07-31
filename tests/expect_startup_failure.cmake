execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                SDL_VIDEODRIVER=dummy
                "${ENGINE}"
        WORKING_DIRECTORY "${ENGINE_WORKING_DIRECTORY}"
        RESULT_VARIABLE engine_result
        OUTPUT_VARIABLE engine_stdout
        ERROR_VARIABLE engine_stderr
)

set(engine_output "${engine_stdout}${engine_stderr}")

if(NOT engine_result STREQUAL "1")
    message(FATAL_ERROR
            "Expected engine exit code 1, got '${engine_result}'.\n"
            "${engine_output}")
endif()

if(NOT engine_output MATCHES
        "Platform initialization failed: Failed to create")
    message(FATAL_ERROR
            "Engine did not report the expected platform startup failure.\n"
            "${engine_output}")
endif()

if(engine_output MATCHES "Invalid device|dumped core")
    message(FATAL_ERROR
            "Engine reached an invalid Vulkan state during startup failure.\n"
            "${engine_output}")
endif()
