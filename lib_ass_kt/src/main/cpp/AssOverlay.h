#ifndef LIBASS_ANDROID_ASS_OVERLAY_H
#define LIBASS_ANDROID_ASS_OVERLAY_H

#include <jni.h>

// Native side of AssRenderType.EFFECTS_ATLAS (see AssOverlay.c for the full design). Declared here
// so AssKt.c can register these with the AssRender JNI method table without needing the rest of
// the atlas implementation visible.

jlong nativeAssOverlayDraw(JNIEnv* env, jclass clazz, jlong overlay, jlong render, jlong track,
    jint fbo, jint frameWidth, jint frameHeight, jint renderWidth, jint renderHeight, jlong timeMs);

void nativeAssOverlayRelease(JNIEnv* env, jclass clazz, jlong overlay);

#endif // LIBASS_ANDROID_ASS_OVERLAY_H
