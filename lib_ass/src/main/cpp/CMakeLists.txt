cmake_minimum_required(VERSION 4.2.0)
project("lib_ass")

set(NATIVE_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/build_native_lib/${ANDROID_ABI}/lib)

if(NOT EXISTS ${NATIVE_LIB_DIR}/libass.so)
    # ANDROID_NDK is <Android SDK path>/ndk/<ndkVersion>, the same layout build_libass_android.py expects.
    cmake_path(GET ANDROID_NDK PARENT_PATH ANDROID_SDK_PATH)
    cmake_path(GET ANDROID_SDK_PATH PARENT_PATH ANDROID_SDK_PATH)
    message(FATAL_ERROR
        "${NATIVE_LIB_DIR}/libass.so doesn't exist. "
        "Build the native libraries first with: "
        "python \"${CMAKE_CURRENT_SOURCE_DIR}/build_libass_android.py\" --sdk-path \"${ANDROID_SDK_PATH}\"")
endif()

file(GLOB NATIVE_LIBS CONFIGURE_DEPENDS ${NATIVE_LIB_DIR}/*.so)
set(LIBASS_DEPENDENCIES "")
foreach(lib_path IN LISTS NATIVE_LIBS)
    get_filename_component(lib_name ${lib_path} NAME_WE)
    string(REGEX REPLACE "^lib" "" lib_name ${lib_name})

    add_library(${lib_name} SHARED IMPORTED)
    set_target_properties(${lib_name} PROPERTIES IMPORTED_LOCATION ${lib_path})

    if(NOT lib_name STREQUAL "ass")
        list(APPEND LIBASS_DEPENDENCIES ${lib_name})
    endif()
endforeach()

# AGP only packages an imported library into jni/ if it is a Prefab module (here only "ass"),
# together with its runtime files a.k.a. the shared libraries in its INTERFACE_LINK_LIBRARIES.
# Due to that, we must add libass dependencies here:
set_target_properties(ass PROPERTIES INTERFACE_LINK_LIBRARIES "${LIBASS_DEPENDENCIES}")
