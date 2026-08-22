add_library(Editor STATIC
        editor_session.cpp
        editor_camera_controller.cpp
)

target_include_directories(Editor PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(Editor PUBLIC
        Camera
        GameObject
        InputManager
        Scene
        Time
        glm
)
