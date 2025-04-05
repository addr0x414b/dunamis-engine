add_subdirectory(debugger)

add_library(dunamis "")

target_sources(dunamis
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/dunamis.cpp
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/dunamis.h
)

target_include_directories(dunamis
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(dunamis
    PUBLIC
        debugger
        visual_server
        vulkan_context
        assimp)