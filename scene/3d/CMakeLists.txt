add_library(game_object "")

target_sources(game_object
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/game_object.cpp
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/game_object.h
)

target_include_directories(game_object
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}
)

find_package(glm CONFIG REQUIRED)
target_link_libraries(game_object PUBLIC glm::glm)

find_package(Vulkan REQUIRED)
target_link_libraries(game_object PUBLIC Vulkan::Vulkan)

add_library(camera "")

target_sources(camera
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/camera.cpp
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/camera.h
)

target_include_directories(camera
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(game_object PUBLIC camera)
target_link_libraries(camera PUBLIC glm::glm)
target_link_libraries(game_object PUBLIC debugger)

find_package(assimp CONFIG REQUIRED)
target_link_libraries(game_object PUBLIC assimp::assimp)