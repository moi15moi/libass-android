# lib_ass/CMakeLists.txt
cmake_minimum_required(VERSION 3.22.1)
project("lib_ass")

add_library(ass SHARED IMPORTED)
set_target_properties(ass PROPERTIES
        IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libass.so)
