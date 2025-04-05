add_library(editor "")

target_sources(editor
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/editor.cpp
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/editor.h
)

target_include_directories(editor
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(editor
    PRIVATE
        ${SDL2_LIBRARIES}
        vulkan_context
        scene
        camera
        imgui
)