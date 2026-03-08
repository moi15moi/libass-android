LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := ass
LOCAL_SRC_FILES := ../jniLibs/$(TARGET_ARCH_ABI)/libass.so
include $(PREBUILT_SHARED_LIBRARY)

$(call import-module,prefab/ass)
