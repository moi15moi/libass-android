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



set(libass_location "//wsl.localhost/Ubuntu/root/we/${ANDROID_ABI}")


set(ENV{PKG_CONFIG_PATH} "${libass_location}/usr/local/lib/pkgconfig")


find_package(PkgConfig REQUIRED)

pkg_search_module(PKG_LIBASS REQUIRED IMPORTED_TARGET libass)


add_library(${CMAKE_PROJECT_NAME} SHARED Ass.c)

target_link_libraries(${CMAKE_PROJECT_NAME}
        "-Wl,--whole-archive" PkgConfig::PKG_LIBASS "-Wl,--no-whole-archive"
        z)