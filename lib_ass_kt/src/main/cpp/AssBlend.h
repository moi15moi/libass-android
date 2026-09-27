#ifndef LIBASS_ANDROID_ASS_BLEND_H
#define LIBASS_ANDROID_ASS_BLEND_H

#include <jni.h>

// Native side of AssRenderType.EFFECTS_ATLAS (see AssBlend.c for the full design). Declared here
// so AssKt.c can register these with the AssRender JNI method table without needing the rest of
// the atlas/worker implementation visible.

jlong nativeAssBlendConfigure(JNIEnv* env, jclass clazz, jlong blend, jlong render, jint renderWidth, jint renderHeight, jint workerWaitMs, jobject renderObj);

jboolean nativeAssBlendWorkerCompute(JNIEnv* env, jclass clazz, jlong blend, jlong render, jlong track, jlong timeMs);

void nativeAssBlendDrawFrame(JNIEnv* env, jclass clazz, jlong blend, jlong timeMs, jint inputTexId);

void nativeAssBlendRelease(JNIEnv* env, jclass clazz, jlong blend);

#endif // LIBASS_ANDROID_ASS_BLEND_H
