# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html.
# For more examples on how to use CMake, see https://github.com/android/ndk-samples.

# Sets the minimum CMake version required for this project.
cmake_minimum_required(VERSION 3.22.1)


# Declares the project name. The project name can be accessed via ${ PROJECT_NAME},
# Since this is the top level CMakeLists.txt, the project name is also accessible
# with ${CMAKE_PROJECT_NAME} (both CMake variables are in-sync within the top level
# build script scope).
project("ass")
# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds them for you.
# Gradle automatically packages shared libraries with your APK.
#s
# In this top level CMakeLists.txt, ${CMAKE_PROJECT_NAME} is used to define
# the target library name; in the sub-module's CMakeLists.txt, ${PROJECT_NAME}
# is preferred for the same purpose.
#

set(libass_location "C:/Users/moi15moi/Documents/GitHub/media/libraries/decoder_ass/src/main/jni/ass/arm64-v8a")
set(libass_binaries "${libass_location}/usr/local/lib")

foreach(libass_lib fribidi harfbuzz freetype expat fontconfig ass)
    set(libass_lib_filename lib${libass_lib}.a)
    set(libass_lib_file_path ${libass_binaries}/${libass_lib_filename})

    add_library(
            test${libass_lib}
            STATIC
            IMPORTED)
    set_target_properties(
            test${libass_lib} PROPERTIES
            IMPORTED_LOCATION
            ${libass_lib_file_path})
endforeach()

include_directories("${libass_location}/usr/local/include")
#include_directories(${CMAKE_BINARY_DIR}/include)



# In order to load a library into your app from Java/Kotlin, you must call
# System.loadLibrary() and pass the name of the library defined here;
# for GameActivity/NativeActivity derived applications, the same library name must be
# used in the AndroidManifest.xml file.
add_library(${CMAKE_PROJECT_NAME} SHARED Ass.c)
#add_dependencies(${CMAKE_PROJECT_NAME} ep_ass)
#set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME "ass")
#target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC ${CMAKE_BINARY_DIR}/include)
#target_link_directories(${CMAKE_PROJECT_NAME} PUBLIC ${CMAKE_BINARY_DIR}/lib)
#if(CMAKE_BUILD_TYPE STREQUAL "Debug")
#    set(FREE_TYPE_NAME "freetyped")
#else()
#    set(FREE_TYPE_NAME "freetype")
#endif()
#target_link_libraries(${CMAKE_PROJECT_NAME}
#        "-Wl,--whole-archive" libass.a fribidi ${FREE_TYPE_NAME} harfbuzz unibreak png "-Wl,--no-whole-archive"
#        z)

target_link_libraries(${CMAKE_PROJECT_NAME}
        "-Wl,--whole-archive"
        testass
        testfribidi
        testharfbuzz
        testfreetype
        testfontconfig
        testexpat
        "-Wl,--no-whole-archive")
