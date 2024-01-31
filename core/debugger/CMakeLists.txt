add_library(debugger "")

target_sources(debugger
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/debugger.cpp
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/debugger.h
)

target_include_directories(debugger
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}
)