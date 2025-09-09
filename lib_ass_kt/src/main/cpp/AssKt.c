#include <jni.h>
#include <android/bitmap.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <jni.h>
#include <EGL/egl.h>   // On Android you already have EGL context active
#include <GLES3/gl3.h>
#include "ass/ass.h"
#include "fontconfig/fontconfig.h"
#include "libplacebo/log.h"
#include "libplacebo/renderer.h"
#include "libplacebo/opengl.h"
#include "libplacebo/utils/upload.h"

#define LOG_TAG "SubtitleRenderer"

void assMessageCallback(int level, const char *fmt, va_list args, void *data) {
    if (level > 4) return;

    if (level >= 2) {
        __android_log_vprint(ANDROID_LOG_WARN, LOG_TAG, fmt, args);
    } else {
        __android_log_vprint(ANDROID_LOG_ERROR, LOG_TAG, fmt, args);
    }
}

jlong nativeAssInit(JNIEnv* env, jclass clazz) {
    ASS_Library* assLibrary = ass_library_init();
    ass_set_message_cb(assLibrary, assMessageCallback, env);
    ass_set_extract_fonts(assLibrary, 1);
    return (jlong) assLibrary;
}

void nativeAssAddFont(JNIEnv* env, jclass clazz, jlong ass, jstring name, jbyteArray byteArray) {
    jsize length = (*env)->GetArrayLength(env, byteArray);

    jbyte* bytePtr = (*env)->GetByteArrayElements(env, byteArray, NULL);

    if (bytePtr == NULL) {
        return;
    }
    const char * cName = (*env)->GetStringUTFChars(env, name, NULL);
    ass_add_font(((ASS_Library *) ass), cName, bytePtr, length);
    (*env)->ReleaseByteArrayElements(env, byteArray, bytePtr, 0);
    if (cName != NULL) {
        (*env)->ReleaseStringUTFChars(env, name, cName);
    }
}

void nativeAssClearFont(JNIEnv* env, jclass clazz, jlong ass) {
    ass_clear_fonts((ASS_Library *) ass);
}

void nativeAssDeinit(JNIEnv* env, jclass clazz, jlong ass) {
    if (ass) {
        ass_library_done((ASS_Library *) ass);
    }
}

static JNINativeMethod method_table[] = {
        {"nativeAssInit", "()J", (void*)nativeAssInit},
        {"nativeAssAddFont", "(JLjava/lang/String;[B)V", (void*) nativeAssAddFont},
        {"nativeAssClearFont", "(J)V", (void*) nativeAssClearFont},
        {"nativeAssDeinit", "(J)V", (void*)nativeAssDeinit}
};

jlong nativeAssTrackInit(JNIEnv* env, jclass clazz, jlong ass) {
    return (jlong) ass_new_track((ASS_Library *) ass);
}

jint nativeAssTrackGetWidth(JNIEnv* env, jclass clazz, jlong track) {
    return ((ASS_Track *) track)->PlayResX;
}

jobjectArray nativeAssTrackGetEvents(JNIEnv* env, jclass clazz, jlong track) {
    jclass eventClass = (*env)->FindClass(env, "io/github/peerless2012/ass/AssEvent");
    if (eventClass == NULL) {
        return NULL;
    }

    jmethodID constructor = (*env)->GetMethodID(env, eventClass, "<init>", "(JJIIILjava/lang/String;IIILjava/lang/String;Ljava/lang/String;)V");
    if (constructor == NULL) {
        return NULL;
    }
    ASS_Track *assTrack = (ASS_Track *) track;

    if (assTrack->n_events <= 0) {
        return NULL;
    }

    jobjectArray eventArray = (*env)->NewObjectArray(env, assTrack->n_events, eventClass, NULL);
    if (eventArray == NULL) {
        return NULL;
    }
    for (int i = 0; i < assTrack->n_events; ++i) {
        ASS_Event assEvent = assTrack->events[i];
        jstring name = (*env)->NewStringUTF(env, assEvent.Name ? assEvent.Name : "");
        jstring effect = (*env)->NewStringUTF(env, assEvent.Effect ? assEvent.Effect : "");
        jstring text = (*env)->NewStringUTF(env, assEvent.Text ? assEvent.Text : "");

        jobject javaEvent = (*env)->NewObject(env, eventClass, constructor,
                                              (jlong) assEvent.Start,
                                              (jlong) assEvent.Duration,
                                              (jint) assEvent.ReadOrder,
                                              (jint) assEvent.Layer,
                                              (jint) assEvent.Style,
                                              name,
                                              (jint) assEvent.MarginL,
                                              (jint) assEvent.MarginR,
                                              (jint) assEvent.MarginV,
                                              effect,
                                              text);

        (*env)->DeleteLocalRef(env, name);
        (*env)->DeleteLocalRef(env, effect);
        (*env)->DeleteLocalRef(env, text);

        (*env)->SetObjectArrayElement(env, eventArray, i, javaEvent);
    }
    return eventArray;
}

void nativeAssTrackClearEvents(JNIEnv* env, jclass clazz, jlong track) {
    ASS_Track* tr = (ASS_Track *) track;
    for (int i = 0; i < tr->n_events; i++) {
        ass_free_event(tr, i);
    }
    tr->n_events = 0;
}

jint nativeAssTrackGetHeight(JNIEnv* env, jclass clazz, jlong track) {
    return ((ASS_Track *) track)->PlayResY;
}

void nativeAssTrackReadBuffer(JNIEnv* env, jclass clazz, jlong track, jbyteArray buffer, jint offset, jint length) {
    jboolean isCopy;
    jbyte* elements = (*env)->GetByteArrayElements(env, buffer, &isCopy);
    if (elements == NULL) {
        return;
    }
    ass_process_data((ASS_Track *) track, elements + offset, length);
    (*env)->ReleaseByteArrayElements(env, buffer, elements, 0);
}

void nativeAssTrackReadChunk(JNIEnv* env, jclass clazz, jlong track, jlong start, jlong duration, jbyteArray buffer, jint offset, jint length) {
    jboolean isCopy;
    jbyte* elements = (*env)->GetByteArrayElements(env, buffer, &isCopy);
    if (elements == NULL) {
        return;
    }
    ass_process_chunk((ASS_Track *) track, elements + offset, length, start, duration);
    (*env)->ReleaseByteArrayElements(env, buffer, elements, 0);
}

void nativeAssTrackDeinit(JNIEnv* env, jclass clazz, jlong track) {
    ass_free_track((ASS_Track *) track);
}


static JNINativeMethod trackMethodTable[] = {
        {"nativeAssTrackInit", "(J)J", (void*)nativeAssTrackInit},
        {"nativeAssTrackGetWidth", "(J)I", (void*) nativeAssTrackGetWidth},
        {"nativeAssTrackGetHeight", "(J)I", (void*) nativeAssTrackGetHeight},
        {"nativeAssTrackGetEvents", "(J)[Lio/github/peerless2012/ass/AssEvent;", (void*) nativeAssTrackGetEvents},
        {"nativeAssTrackClearEvents", "(J)V", (void*) nativeAssTrackClearEvents},
        {"nativeAssTrackReadBuffer", "(J[BII)V", (void*)nativeAssTrackReadBuffer},
        {"nativeAssTrackReadChunk", "(JJJ[BII)V", (void*)nativeAssTrackReadChunk},
        {"nativeAssTrackDeinit", "(J)V", (void*)nativeAssTrackDeinit}
};

jlong nativeAssRenderInit(JNIEnv* env, jclass clazz, jlong ass) {
    ASS_Renderer *assRenderer = ass_renderer_init((ASS_Library *) ass);
    ass_set_fonts(assRenderer, NULL, "sans-serif", ASS_FONTPROVIDER_FONTCONFIG, NULL, 1);
    return (jlong) assRenderer;
}

void nativeAssRenderSetFontScale(JNIEnv* env, jclass clazz, jlong render, jfloat scale) {
    ass_set_font_scale((ASS_Renderer *) render, scale);
}

void nativeAssRenderSetCacheLimit(JNIEnv* env, jclass clazz, jlong render, jint glyphMax, jint bitmapMaxSize) {
    ass_set_cache_limits((ASS_Renderer *) render, glyphMax, bitmapMaxSize);
}

void nativeAssRenderSetFrameSize(JNIEnv* env, jclass clazz, jlong render, jint width, jint height) {
    ass_set_frame_size((ASS_Renderer *) render, width, height);
}

void nativeAssRenderSetStorageSize(JNIEnv* env, jclass clazz, jlong render, jint width, jint height) {
    ass_set_storage_size((ASS_Renderer *) render, width, height);
}

jobject createBitmap(JNIEnv* env, const ASS_Image* image) {
    jclass bitmapConfigClass = (*env)->FindClass(env, "android/graphics/Bitmap$Config");
    jfieldID argb8888FieldID = (*env)->GetStaticFieldID(env, bitmapConfigClass, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
    jobject argb8888 = (*env)->GetStaticObjectField(env, bitmapConfigClass, argb8888FieldID);

    jclass bitmapClass = (*env)->FindClass(env, "android/graphics/Bitmap");
    jmethodID createBitmapMethodID = (*env)->GetStaticMethodID(env,
                                                               bitmapClass, "createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    jobject bitmap = (*env)->CallStaticObjectMethod(env,
                                                    bitmapClass, createBitmapMethodID, image->w, image->h, argb8888);

    void* bitmapPixels;
    AndroidBitmap_lockPixels(env, bitmap, &bitmapPixels);
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) < 0) {
        AndroidBitmap_unlockPixels(env, bitmap);
        return NULL;
    }

    int stride = image->stride;
    unsigned int r = (image->color >> 24) & 0xFF;
    unsigned int g = (image->color >> 16) & 0xFF;
    unsigned int b = (image->color >> 8) & 0xFF;
    unsigned int opacity = 0xFF - image->color & 0xFF;
    for (int y = 0; y < image->h; ++y) {
        uint32_t *line = (uint32_t *)((char *)bitmapPixels + (y) * info.stride);
        for (int x = 0; x < image->w; ++x) {
            unsigned alpha = image->bitmap[y * stride + x];
            if (alpha > 0) {
                unsigned int a = (opacity * alpha) / 255;
                // premultiplied alpha
                float pm = a / 255.0f;
                // ABGR
                line[x] = a << 24 | ((unsigned int) (b * pm) << 16) | ((unsigned int) (g * pm) << 8) | (unsigned int) (r * pm);
            } else {
                line[x] = 0;
            }
        }
    }
    AndroidBitmap_unlockPixels(env, bitmap);

    return bitmap;
}

jobject createAlphaBitmap(JNIEnv* env, const ASS_Image* image) {
    jclass bitmapConfigClass = (*env)->FindClass(env, "android/graphics/Bitmap$Config");
    jfieldID alpha8FieldId = (*env)->GetStaticFieldID(env, bitmapConfigClass, "ALPHA_8", "Landroid/graphics/Bitmap$Config;");
    jobject alpha8 = (*env)->GetStaticObjectField(env, bitmapConfigClass, alpha8FieldId);

    jclass bitmapClass = (*env)->FindClass(env, "android/graphics/Bitmap");
    jmethodID createBitmapMethodID = (*env)->GetStaticMethodID(env,
                                                               bitmapClass, "createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    jobject bitmap = (*env)->CallStaticObjectMethod(env,
                                                    bitmapClass, createBitmapMethodID, image->w, image->h, alpha8);

    void* bitmapPixels;
    AndroidBitmap_lockPixels(env, bitmap, &bitmapPixels);
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) < 0) {
        AndroidBitmap_unlockPixels(env, bitmap);
        return NULL;
    }

    if (info.stride == image->stride) {
        memcpy(bitmapPixels, image->bitmap, info.stride * info.height);
    } else {
        for (int y = 0; y < image->h; ++y) {
            char *dst = (char *) bitmapPixels + y * info.stride;
            char *src = (char *) image->bitmap + y * image->stride;
            memcpy(dst, src, image->w);
        }
    }
    AndroidBitmap_unlockPixels(env, bitmap);

    return bitmap;
}

static int count_ass_images(ASS_Image *images) {
    int count = 0;
    for (ASS_Image *img = images; img != NULL; img = img->next) {
        count++;
    }
    return count;
}

static void log_cb(void *priv, enum pl_log_level level, const char *msg)
{
    __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "placebo - %s", msg);
}

void checkGLError(const char* context) {
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "OpenGL error at %s: 0x%04x", context, err);
    }
}


typedef struct libplacebo_context {
    pl_log pllog;
    pl_opengl plgl;
    pl_renderer renderer;
    pl_fmt format_r8;
    pl_fmt format_rgba8;
} LibplaceboContext;


jlong nativeInitializeLibplacebo(JNIEnv* env, jclass clazz) {
    EGLDisplay display = eglGetCurrentDisplay();
    EGLContext context = eglGetCurrentContext();
    if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "Failed to eglGetCurrentDisplay or eglGetCurrentContext");
        return 0;
    }

    pl_log pllog = pl_log_create(PL_API_VER, &(struct pl_log_params) {
            .log_cb     = log_cb,
            .log_level   = PL_LOG_DEBUG,
    });

    struct pl_opengl_params gl_params = {
            .get_proc_addr = (void*)eglGetProcAddress,
            .allow_software     = true,         // allow software rasterers
            .debug              = true,         // enable error reporting
    };
    pl_opengl plgl = pl_opengl_create(pllog, &gl_params);
    if (!plgl) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "Failed to create pl_opengl");
        return 0;
    }

    pl_renderer renderer = pl_renderer_create(pllog, plgl->gpu);
    if (!renderer) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "Failed to create renderer");
        pl_opengl_destroy(&plgl);
        return 0;
    }

    pl_fmt format_r8 = pl_find_named_fmt(plgl->gpu, "r8");
    if (!format_r8) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "Format r8 not found");
        return 0;
    }

    pl_fmt format_rgba8 = pl_find_named_fmt(plgl->gpu, "rgba8");
    if (!format_rgba8) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "Format rgba8 not found");
        return 0;
    }


    LibplaceboContext* libplaceboContext = (LibplaceboContext*)malloc(sizeof(LibplaceboContext));
    if (!libplaceboContext) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "Failed to allocate LibplaceboContext");
        return 0;
    }

    libplaceboContext->pllog = pllog;
    libplaceboContext->plgl = plgl;
    libplaceboContext->renderer = renderer;
    libplaceboContext->format_r8 = format_r8;
    libplaceboContext->format_rgba8 = format_rgba8;

    // Return pointer casted to jlong
    return (jlong)libplaceboContext;
}

void nativeUninitializeLibplacebo(JNIEnv* env, jclass clazz, jlong ctxPtr) {
    if (!ctxPtr) return;

    LibplaceboContext* ctx = (LibplaceboContext*)ctxPtr;

    if (ctx->renderer) {
        pl_renderer_destroy(&ctx->renderer);
    }

    if (ctx->plgl) {
        pl_opengl_destroy(&ctx->plgl);
    }

    if (ctx->pllog) {
        pl_log_destroy(&ctx->pllog);
    }


    free(ctx);
}


jobject nativeAssRenderFrame(JNIEnv* env, jclass clazz, jlong context, jlong render, jlong track, jlong time, jboolean onlyAlpha, jint width, jint height) {
    if (!context) return NULL;

    LibplaceboContext* ctx = (LibplaceboContext*)context;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int changed;
    ASS_Image *image = ass_render_frame((ASS_Renderer *) render, (ASS_Track *) track, time, &changed);
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0 +
                        (end.tv_nsec - start.tv_nsec) / 1000000.0;

    __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "ass_render_frame took %.3f ms", elapsed_ms);
    __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "ass_render_frame resolution %ix%i", width, height);

    if (image == NULL) {
        return NULL;
    }

    struct pl_tex_params out_params = {
            .w = image->w,
            .h = image->h,
            .format = ctx->format_r8,
            .renderable = true,
            .host_readable = true,
            .host_writable = true,
            .blit_dst = true,
            .sampleable = true,
    };
    pl_tex src_tex = pl_tex_create(ctx->plgl->gpu, &out_params);
    if (!src_tex) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "Failed to create output texture");
        return NULL;
    }

    // TODO Utiliser posix_memalign
    bool is_ok = pl_tex_upload(ctx->plgl->gpu, &(struct pl_tex_transfer_params) {
            .tex        = src_tex,
            .rc         = { .x1 = image->w, .y1 = image->h, },
            .row_pitch  = image->stride,
            .ptr        = image->bitmap,
    });
    if (!is_ok) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "Failed to pl_tex_upload");
        pl_tex_destroy(ctx->plgl->gpu, &src_tex);
        return NULL;
    }

    struct pl_overlay_part part = {
            .src = { image->dst_x, image->dst_y, image->dst_x + image->w, image->dst_y + image->h },
            .dst = { image->dst_x, image->dst_y, image->dst_x + image->w, image->dst_y + image->h },
            .color = {
                    (image->color >> 24) / 255.0f,
                    ((image->color >> 16) & 0xFF) / 255.0f,
                    ((image->color >> 8) & 0xFF) / 255.0f,
                    (255 - (image->color & 0xFF)) / 255.0f,
            }
    };

    struct pl_overlay overlayl = {
            .tex = src_tex,
            .parts = &part,
            .mode = PL_OVERLAY_MONOCHROME,
            .num_parts = 1,
            .color = {
                    .primaries = PL_COLOR_PRIM_BT_709,
                    .transfer = PL_COLOR_TRC_SRGB,
            },
            .repr = {
                    .alpha = PL_ALPHA_INDEPENDENT
            }
    };


    struct pl_tex_params dst_params = {
            .w = width,
            .h = height,
            .format = ctx->format_rgba8,
            .renderable = true,
            .host_readable = true,
            .host_writable = true,
            .blit_dst = true,
            .sampleable = true,
    };
    pl_tex dst_tex = pl_tex_create(ctx->plgl->gpu, &dst_params);
    if (!dst_tex) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "Failed to create output texture");
        return NULL;
    }

    unsigned int target_type, iformat, fbo;
    GLuint tex_id = pl_opengl_unwrap(ctx->plgl->gpu, dst_tex, &target_type, (int *)&iformat, &fbo);

    __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "Libplacebo output GL texture ID: %u", tex_id);

    struct pl_frame target = {
            .repr = pl_color_repr_rgb,
            .num_planes = 1,
            .planes[0] = {
                    .texture = dst_tex,
                    .components = 4,
                    .component_mapping = {0, 1, 2, 3},
            },
            .overlays = &overlayl,
            .num_overlays = 1,
    };
    is_ok = pl_render_image(ctx->renderer, NULL, &target, &pl_render_default_params);
    if (!is_ok) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "Failed to pl_render_image");
        pl_tex_destroy(ctx->plgl->gpu, &src_tex);
        return NULL;
    }

    jclass integerClass = (*env)->FindClass(env, "java/lang/Integer");
    jmethodID constructor = (*env)->GetMethodID(env, integerClass, "<init>", "(I)V");
    return (*env)->NewObject(env, integerClass, constructor, (jint) tex_id);
}

void nativeAssRenderDeinit(JNIEnv* env, jclass clazz, jlong render) {
    if (render) {
        ass_renderer_done((ASS_Renderer *) render);
    }
}

static JNINativeMethod renderMethodTable[] = {
        {"nativeAssRenderInit", "(J)J", (void*)nativeAssRenderInit},
        {"nativeAssRenderSetFontScale", "(JF)V", (void*)nativeAssRenderSetFontScale},
        {"nativeAssRenderSetCacheLimit", "(JII)V", (void*)nativeAssRenderSetCacheLimit},
        {"nativeAssRenderSetStorageSize", "(JII)V", (void*) nativeAssRenderSetStorageSize},
        {"nativeAssRenderSetFrameSize", "(JII)V", (void*)nativeAssRenderSetFrameSize},
        {"nativeAssRenderFrame", "(JJJJZII)Ljava/lang/Integer;", (void*) nativeAssRenderFrame},
        {"nativeAssRenderDeinit", "(J)V", (void*)nativeAssRenderDeinit},
        {"nativeInitializeLibplacebo", "()J", (void*)nativeInitializeLibplacebo},
        {"nativeUninitializeLibplacebo", "(J)V", (void*)nativeUninitializeLibplacebo},
};
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env = NULL;
    jint result = -1;

    if ((*vm)->GetEnv(vm, (void **) &env, JNI_VERSION_1_4) != JNI_OK) {
        return -1;
    }
    jclass clazz = (*env)->FindClass(env, "io/github/peerless2012/ass/Ass");
    if (clazz == NULL) {
        return -1;
    }

    if ((*env)->RegisterNatives(env, clazz, method_table, sizeof(method_table) / sizeof(method_table[0])) < 0) {
        return -1;
    }

    clazz = (*env)->FindClass(env, "io/github/peerless2012/ass/AssTrack");
    if (clazz == NULL) {
        return -1;
    }

    if ((*env)->RegisterNatives(env, clazz, trackMethodTable, sizeof(trackMethodTable) / sizeof(trackMethodTable[0])) < 0) {
        return -1;
    }

    clazz = (*env)->FindClass(env, "io/github/peerless2012/ass/AssRender");
    if (clazz == NULL) {
        return -1;
    }

    if ((*env)->RegisterNatives(env, clazz, renderMethodTable, sizeof(renderMethodTable) / sizeof(renderMethodTable[0])) < 0) {
        return -1;
    }

    result = JNI_VERSION_1_6;
    return result;
}