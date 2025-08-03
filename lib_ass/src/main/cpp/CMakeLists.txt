# lib_ass/CMakeLists.txt
cmake_minimum_required(VERSION 3.22.1)
project("lib_ass")

set(libass_location "${CMAKE_SOURCE_DIR}/${ANDROID_ABI}")
set(ENV{PKG_CONFIG_PATH} "${libass_location}/lib/pkgconfig")

find_package(PkgConfig REQUIRED)
pkg_search_module(PKG_LIBASS REQUIRED IMPORTED_TARGET libass)

# Define our own interface target that wraps PkgConfig one
add_library(lib_ass_interface INTERFACE)

# Forward include directories and link dependencies
target_include_directories(lib_ass_interface INTERFACE "${libass_location}/include")
target_link_libraries(lib_ass_interface INTERFACE PkgConfig::PKG_LIBASS)

# Optionally give it an alias name for external use
add_library(lib_ass::ass ALIAS lib_ass_interface)
