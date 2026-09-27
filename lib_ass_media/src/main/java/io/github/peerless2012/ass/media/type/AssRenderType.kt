package io.github.peerless2012.ass.media.type

/**
 * ASS render type
 */
enum class AssRenderType {

    /**
     * Use SubtitleView render.
     */
    CUES,

    /**
     * Use Effect(Powered by canvas)
     */
    @Deprecated("Use OVERLAY instead.")
    EFFECTS_CANVAS,

    /**
     * Use Effect(Powered by OpenGL)
     */
    @Deprecated("Use OVERLAY instead.")
    EFFECTS_OPEN_GL,

    /**
     * Use Effect(Powered by a native GLES `GlEffect`/`GlShaderProgram` with a GPU texture atlas).
     *
     * All subtitle rendering and blending (packing, atlas upload, video blit, overlay draw) is
     * done natively in a single JNI call per frame, baked directly into ExoPlayer's video effects
     * pipeline. Fastest available blending path, at the cost of the same HDR/DV limitation as
     * [EFFECTS_OPEN_GL] (relies on a plain 2D GL texture pipeline).
     */
    EFFECTS_ATLAS,

    /**
     * Use Widget overlay(Powered by Canvas).
     */
    OVERLAY_CANVAS,

    /**
     * Use Widget overlay(Powered by OPEN GL).
     */
    OVERLAY_OPEN_GL,
}