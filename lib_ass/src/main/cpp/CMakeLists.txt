# lib_ass/CMakeLists.txt
cmake_minimum_required(VERSION 3.22.1)
project("lib_ass")

#set(libass_location "${CMAKE_SOURCE_DIR}/${ANDROID_ABI}")
#set(ENV{PKG_CONFIG_PATH} "${libass_location}/lib/pkgconfig")

#find_package(PkgConfig REQUIRED)
#pkg_search_module(PKG_LIBASS REQUIRED IMPORTED_TARGET libass)


# From https://developer.android.com/studio/projects/configure-cmake#add-other-library

add_library(libass
        SHARED
        IMPORTED)

set_target_properties(
        libass
        PROPERTIES IMPORTED_LOCATION
        ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libass.so)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/build_native_lib/include)
