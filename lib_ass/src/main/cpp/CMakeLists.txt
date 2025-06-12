# Minimum required version of CMake
cmake_minimum_required(VERSION 3.22.1)

# Project name
project("ass")

# Add libass CMake project
add_subdirectory(libass-cmake)

# Include directory for headers
include_directories(${CMAKE_BINARY_DIR}/include)

# Create shared library from Ass.c
add_library(${CMAKE_PROJECT_NAME} SHARED Ass.c)

# Ensure libass is built before our target
add_dependencies(${CMAKE_PROJECT_NAME} ep_ass)

# Add include and library paths to target
target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC ${CMAKE_BINARY_DIR}/include)
target_link_directories(${CMAKE_PROJECT_NAME} PUBLIC ${CMAKE_BINARY_DIR}/lib)

# Link against required libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    "-Wl,--whole-archive" 
    expat fontconfig libass.a fribidi freetype harfbuzz unibreak 
    "-Wl,--no-whole-archive"
    z
)
