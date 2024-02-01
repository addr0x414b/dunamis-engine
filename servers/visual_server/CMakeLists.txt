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

find_package(SDL2 CONFIG REQUIRED)
target_link_libraries(visual_server
    PRIVATE
    $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
    $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
)

target_link_libraries(visual_server PRIVATE vulkan_context)
target_link_libraries(visual_server PRIVATE editor)
target_link_libraries(visual_server PRIVATE scene)
target_link_libraries(visual_server PRIVATE game_object)