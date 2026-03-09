# lib_ass/CMakeLists.txt
cmake_minimum_required(VERSION 3.22.1)
project("lib_ass")

# Prefab needs a built target "ass", not IMPORTED. Build stub then replace with prebuilt.
set(ASS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libass.so)
add_library(ass SHARED ass_stub.c)
set_target_properties(ass PROPERTIES
        OUTPUT_NAME ass
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
add_custom_command(TARGET ass POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ASS_SRC} $<TARGET_FILE:ass>
        COMMENT "Replacing with prebuilt libass.so")
