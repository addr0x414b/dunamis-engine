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

target_link_libraries(editor PRIVATE vulkan_context)

find_package(imgui CONFIG REQUIRED)
target_link_libraries(editor PRIVATE imgui::imgui)
target_link_libraries(editor PRIVATE scene)
target_link_libraries(editor PRIVATE camera)

find_package(SDL2 CONFIG REQUIRED)
target_link_libraries(editor
    PRIVATE
    $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
    $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
)