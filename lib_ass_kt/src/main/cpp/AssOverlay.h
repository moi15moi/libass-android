#ifndef LIBASS_ANDROID_ASS_OVERLAY_H
#define LIBASS_ANDROID_ASS_OVERLAY_H

#include <jni.h>

// Native side of AssRenderType.EFFECTS_ATLAS. The `overlay` handle is an opaque pointer owned by
// AssOverlay.c: 0 on the first draw call, then whatever the previous draw call returned.

jlong nativeAssOverlayDraw(JNIEnv* env, jclass clazz, jlong overlay, jlong render, jlong track,
    jint fbo, jint frameWidth, jint frameHeight, jlong timeMs);

void nativeAssOverlayRelease(JNIEnv* env, jclass clazz, jlong overlay);

#endif // LIBASS_ANDROID_ASS_OVERLAY_H
