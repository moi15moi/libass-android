// Host stand-in for <jni.h>: just the types in AssOverlay.h's signatures. The tools call those
// functions directly, without a JVM, passing NULL for the JNIEnv and jclass.
#pragma once

#include <stdint.h>

typedef int32_t jint;
typedef int64_t jlong;
typedef void* jclass;
typedef const struct JNINativeInterface* JNIEnv;
