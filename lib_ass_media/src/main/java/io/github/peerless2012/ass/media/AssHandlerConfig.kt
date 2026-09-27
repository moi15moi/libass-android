package io.github.peerless2012.ass.media

data class AssHandlerConfig(
    val glyphSize: Int = 10000,
    val cacheSize: Int = 128,

    /**
     * Maximum number of pixels (width * height) for subtitle rendering.
     *
     * When the target frame size exceeds this limit, the render size will be
     * proportionally downscaled while maintaining the aspect ratio.
     *
     * This reduces CPU and memory usage on high-resolution displays (e.g., 4K TVs)
     * at the cost of slightly lower subtitle sharpness.
     *
     * Examples:
     * - 1920 * 1080 = 2_073_600 (limit to 1080p)
     * - 2560 * 1440 = 3_686_400 (limit to 1440p)
     * - 0 = no limit, render at full frame size (default)
     *
     * Only applies to OVERLAY and EFFECTS render types. CUES mode is not affected.
     */
    val maxRenderPixels: Int = 0,

    /**
     * How long, in milliseconds, [io.github.peerless2012.ass.media.type.AssRenderType.EFFECTS_ATLAS]'s
     * GL thread will wait per frame for its native worker thread to finish computing the exact
     * subtitle state requested, before falling back to whatever it last published (see
     * `AssBlend.c`'s `nativeAssBlendDrawFrame`).
     *
     * This trades frame-perfect accuracy against never stalling video delivery. Tune it against your
     * content's actual `ass_render_frame` + atlas-pack cost (log tag `AssBlend`, e.g.
     * `adb logcat -s AssBlend:V`) relative to your video's frame period:
     * - Too low: every frame that takes longer than this to compute gets drawn one request behind
     *   (visible as `TIMED OUT ... drawing stale atlas` in logcat), even though the worker may well
     *   be keeping up overall.
     * - Too high, especially on higher-frame-rate video: a slow/complex subtitle frame can stall
     *   video delivery for up to this long, defeating the whole point of computing off the GL thread.
     *
     * The default (16ms) is a safe margin under a 60fps frame period (~16.7ms). Content with a lower
     * frame rate (e.g. 24fps, ~42ms per frame) has much more headroom and can often raise this
     * comfortably above its own measured render cost for consistently frame-accurate subtitles.
     *
     * Only applies to EFFECTS_ATLAS.
     */
    val blendWorkerWaitMs: Int = 16
)
