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
     * Subtitles are drawn directly onto each decoded video frame's own texture, in place — no
     * separate output texture, no full-frame video copy — natively in a single JNI call per frame,
     * baked directly into ExoPlayer's video effects pipeline. Fastest available path, at the cost
     * of the same HDR/DV limitation as [EFFECTS_OPEN_GL] (relies on a plain 2D GL texture pipeline).
     */
    EFFECTS_ATLAS,

    /**
     * Same idea as [EFFECTS_ATLAS] (in-place GL overlay, no worker thread), but backed by a
     * separate, mostly-unmodified C++ reference implementation (`ass_gl_overlay.cpp`) instead of
     * this library's own native code, kept side by side purely to A/B-compare performance against
     * [EFFECTS_ATLAS]. Not feature-equivalent: it captures whatever subtitle track is current when the effect is created —
     * it does not follow later track changes.
     */
    EFFECTS_ATLAS_CPP,

    /**
     * Use Widget overlay(Powered by Canvas).
     */
    OVERLAY_CANVAS,

    /**
     * Use Widget overlay(Powered by OPEN GL).
     */
    OVERLAY_OPEN_GL,
}