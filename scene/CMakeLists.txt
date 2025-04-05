add_subdirectory(3d)

add_library(scene "")

target_sources(scene
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/scene.cpp
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/scene.h
)

target_include_directories(scene
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(scene 
    PUBLIC 
        camera
        game_object
)