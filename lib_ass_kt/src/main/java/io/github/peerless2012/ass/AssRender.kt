package io.github.peerless2012.ass

import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

/**
 * @Author peerless2012
 * @Email peerless2012@126.com
 * @DateTime 2025/Jan/05 14:18
 * @Version V1.0
 * @Description
 */
class AssRender(nativeAss: Long, private val lock: ReentrantLock) {

    companion object {

        @JvmStatic
        external fun nativeAssRenderInit(ass: Long): Long

        @JvmStatic
        external fun nativeAssRenderSetFontScale(render: Long, scale: Float)

        @JvmStatic
        external fun nativeAssRenderSetCacheLimit(render: Long, glyphMax: Int, bitmapMaxSize: Int)

        @JvmStatic
        external fun nativeAssRenderSetStorageSize(render: Long, width: Int, height: Int)

        @JvmStatic
        external fun nativeAssRenderSetFrameSize(render: Long, width: Int, height: Int)

        @JvmStatic
        external fun nativeAssRenderFrame(render: Long, track: Long, time: Long, type: Int): AssFrame?

        @JvmStatic
        external fun nativeAssRenderDeinit(render: Long)

        @JvmStatic
        external fun nativeAssOverlayDraw(overlay: Long, render: Long, track: Long, fbo: Int, frameWidth: Int, frameHeight: Int, timeMs: Long): Long

        @JvmStatic
        external fun nativeAssOverlayRelease(overlay: Long)
    }

    private var nativeRender: Long = nativeAssRenderInit(nativeAss)

    /**
     * Raw `ASS_Renderer*` pointer. Exposed read-only for callers outside this class's own API
     * surface that need to hand it directly to a native factory function — currently only
     * `EFFECTS_ATLAS_CPP`'s comparison effect (`com.example.subs.AssOverlayProgram.nativeCreate`),
     * which is entirely separate native code this class has no other way to reach into.
     */
    val nativeRenderPtr: Long get() = nativeRender

    /** Native handle for the EFFECTS_ATLAS GlEffect overlay state (atlas texture/program/VBO).
     * GL-thread owned; see [drawOverlayFrame]. */
    private var nativeOverlay: Long = 0L

    @Volatile
    var released = false
        private set

    private var track: AssTrack? = null

    public fun setTrack(track: AssTrack?) {
        lock.withLock {
            this.track = track
        }
    }

    public fun setFontScale(scale: Float) {
        lock.withLock {
            if (released || nativeRender == 0L) return
            nativeAssRenderSetFontScale(nativeRender, scale)
        }
    }

    public fun setCacheLimit(glyphMax: Int, bitmapMaxSize: Int) {
        lock.withLock {
            if (released || nativeRender == 0L) return
            nativeAssRenderSetCacheLimit(nativeRender, glyphMax, bitmapMaxSize)
        }
    }

    public fun setStorageSize(width: Int, height: Int) {
        lock.withLock {
            if (released || nativeRender == 0L) return
            nativeAssRenderSetStorageSize(nativeRender, width, height)
        }
    }

    public fun setFrameSize(width: Int, height: Int) {
        lock.withLock {
            if (released || nativeRender == 0L) return
            nativeAssRenderSetFrameSize(nativeRender, width, height)
        }
    }

    public fun renderFrame(time: Long, type: AssTexType): AssFrame? {
        lock.withLock {
            if (released || nativeRender == 0L) return null
            val t = track ?: return null
            if (t.released || t.nativeAssTrack == 0L) return null
            return nativeAssRenderFrame(nativeRender, t.nativeAssTrack, time, type.ordinal)
        }
    }

    /**
     * Draws ASS subtitles directly into [fbo] (which wraps the video frame's own decoded texture,
     * drawn in place — see `lib_ass_media`'s `AssGlShaderProgram.queueInputFrame`) for [timeMs],
     * synchronously. GL-thread only.
     *
     * Unlike an earlier worker-thread/bounded-wait design this replaced, this always calls
     * `ass_render_frame` directly, inline, on the calling thread: always frame-accurate by
     * construction, at the cost of being able to stall video delivery if a subtitle frame is
     * pathologically slow to render (see `AssOverlay.c`'s file header comment for the full
     * tradeoff). Also unlike that design, a frame with no visible subtitle content does no GL work
     * at all — there's no separate output texture to fill regardless of content.
     *
     * [frameWidth]/[frameHeight] is the video frame's own pixel size: libass storage and frame
     * size, and the GL viewport this draws into. `AssHandlerConfig.maxRenderPixels` does not apply.
     *
     * Runs under [lock] exactly like [renderFrame], so it is safe with respect to concurrent track
     * mutation ([AssTrack.readChunk]) and [release] — the underlying GL objects (atlas
     * texture/program/VBO) are created lazily on first call and reused after, mirroring how
     * `ASS_Renderer`/`ASS_Track` are re-validated each call rather than cached across calls.
     */
    fun drawOverlayFrame(fbo: Int, frameWidth: Int, frameHeight: Int, timeMs: Long) {
        lock.withLock {
            if (released || nativeRender == 0L) return
            val t = track ?: return
            if (t.released || t.nativeAssTrack == 0L) return
            nativeOverlay = nativeAssOverlayDraw(nativeOverlay, nativeRender, t.nativeAssTrack, fbo, frameWidth, frameHeight, timeMs)
        }
    }

    /**
     * Releases the native EFFECTS_ATLAS overlay GL state (atlas texture/program/VBO). Must be
     * called on the GL thread (mirrors [drawOverlayFrame]).
     */
    fun releaseOverlayGl() {
        lock.withLock {
            if (nativeOverlay != 0L) {
                nativeAssOverlayRelease(nativeOverlay)
                nativeOverlay = 0L
            }
        }
    }

    fun release() {
        lock.withLock {
            if (released) return
            released = true
            track = null
            if (nativeRender != 0L) {
                nativeAssRenderDeinit(nativeRender)
                nativeRender = 0
            }
        }
    }

    protected fun finalize() {
        release()
    }

}
