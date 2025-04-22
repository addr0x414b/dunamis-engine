add_library(Level1 STATIC level_1.cpp)
target_include_directories(Level1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Level1 PRIVATE Scene assimp glm spdlog Player)

add_library(Player STATIC player.cpp)
target_include_directories(Player PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Player PRIVATE spdlog GameObject Camera)