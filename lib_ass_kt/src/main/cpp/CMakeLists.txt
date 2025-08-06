# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html.
# For more examples on how to use CMake, see https://github.com/android/ndk-samples.

# Sets the minimum CMake version required for this project.
cmake_minimum_required(VERSION 3.22.1)

# Declares the project name. The project name can be accessed via ${ PROJECT_NAME},
# Since this is the top level CMakeLists.txt, the project name is also accessible
# with ${CMAKE_PROJECT_NAME} (both CMake variables are in-sync within the top level
# build script scope).
project("asskt")
# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds them for you.
# Gradle automatically packages shared libraries with your APK.
#s
# In this top level CMakeLists.txt, ${CMAKE_PROJECT_NAME} is used to define
# the target library name; in the sub-module's CMakeLists.txt, ${PROJECT_NAME}
# is preferred for the same purpose.
#
include_directories(${CMAKE_SOURCE_DIR}/include)
# In order to load a library into your app from Java/Kotlin, you must call
# System.loadLibrary() and pass the name of the library defined here;
# for GameActivity/NativeActivity derived applications, the same library name must be
# used in the AndroidManifest.xml file.
add_library(${CMAKE_PROJECT_NAME} SHARED AssKt.c)
# Add these two lines.
find_package(lib_ass REQUIRED CONFIG)

get_target_property(ass_location lib_ass::libass IMPORTED_LOCATION)
get_target_property(ass_type lib_ass::libass TYPE)
get_target_property(ass_includes lib_ass::libass INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(ass_link_libs lib_ass::libass INTERFACE_LINK_LIBRARIES)

message(STATUS "lib_ass::ass LOCATION: ${ass_location}")
message(STATUS "lib_ass::ass TYPE: ${ass_type}")
message(STATUS "lib_ass::ass INCLUDE DIRS: ${ass_includes}")
message(STATUS "lib_ass::ass LINK LIBS: ${ass_link_libs}")

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME "asskt")
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
        lib_ass::libass
        android
        jnigraphics
        z
        log)