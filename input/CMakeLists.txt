add_library(InputManager STATIC input_manager.cpp)
target_include_directories(InputManager PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(InputManager PRIVATE SDL3::SDL3 spdlog)