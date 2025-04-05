add_library(visual_server "")

target_sources(visual_server
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/visual_server.cpp
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/visual_server.h
)

target_include_directories(visual_server
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(visual_server
    PRIVATE
        vulkan_context
        editor
        scene
        game_object
        imgui
        ${SDL2_LIBRARIES}
)