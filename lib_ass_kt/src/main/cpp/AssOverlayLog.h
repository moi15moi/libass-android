#ifndef LIBASS_ANDROID_ASS_OVERLAY_LOG_H
#define LIBASS_ANDROID_ASS_OVERLAY_LOG_H

#include <android/log.h>

#define ASS_OVERLAY_LOG_TAG "AssOverlay"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, ASS_OVERLAY_LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, ASS_OVERLAY_LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, ASS_OVERLAY_LOG_TAG, __VA_ARGS__)

#endif // LIBASS_ANDROID_ASS_OVERLAY_LOG_H
