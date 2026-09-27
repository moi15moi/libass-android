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
        external fun nativeAssBlendConfigure(blend: Long, render: Long, renderWidth: Int, renderHeight: Int, workerWaitMs: Int, renderObj: AssRender): Long

        @JvmStatic
        external fun nativeAssBlendDrawFrame(blend: Long, timeMs: Long, inputTexId: Int)

        @JvmStatic
        external fun nativeAssBlendWorkerCompute(blend: Long, render: Long, track: Long, timeMs: Long): Boolean

        @JvmStatic
        external fun nativeAssBlendRelease(blend: Long)
    }

    private var nativeRender: Long = nativeAssRenderInit(nativeAss)

    /** Native handle for the EFFECTS_ATLAS GlEffect blend state. GL-thread owned; see [configureGlBlend]. */
    private var nativeBlend: Long = 0L

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
     * Configures (or reconfigures) the native GL blend state used by the EFFECTS_ATLAS `GlEffect`
     * render path (see `lib_ass_media`'s `AssGlShaderProgram`). Must be called on the thread that
     * owns the GL context. Safe to call more than once (e.g. on a video-dimension change) — the
     * underlying GL objects are created lazily on first call and reused after.
     *
     * [workerWaitMs] is how long [drawGlBlendFrame] will bound-wait per frame for the worker to
     * finish (see `lib_ass_media`'s `AssHandlerConfig.blendWorkerWaitMs` for the tradeoff and how to
     * tune it); updated on every call, so later reconfigures can adjust it without needing to fully
     * release and recreate the blend state.
     *
     * On first call, this also starts a dedicated native worker thread that computes
     * `ass_render_frame` continuously, independent of the video frame rate — see
     * [drawGlBlendFrame] and [workerComputeBlendFrame].
     */
    fun configureGlBlend(renderWidth: Int, renderHeight: Int, workerWaitMs: Int): Boolean {
        lock.withLock {
            if (released || nativeRender == 0L) return false
            nativeBlend = nativeAssBlendConfigure(nativeBlend, nativeRender, renderWidth, renderHeight, workerWaitMs, this)
            return nativeBlend != 0L
        }
    }

    /**
     * Draws one blended video+subtitle frame into the currently bound output framebuffer. GL-thread only.
     *
     * This never calls into libass directly — it hands the worker thread the latest requested time
     * and bounded-waits (a few ms, see `ASS_BLEND_WORKER_WAIT_MS` in `AssBlend.c`) for
     * [workerComputeBlendFrame] to finish that exact request before drawing whatever atlas state was
     * published, so normal subtitle frames stay frame-accurate while a pathologically
     * slow/complex one still can't stall video frame delivery past the timeout — it falls back to
     * whatever the worker last published instead. It always issues the video blit even when there
     * is no usable track yet (e.g. before the ASS track has loaded, or media with no subtitles) —
     * only the subtitle overlay pass is skipped in that case, natively, once the worker observes
     * there is no track.
     */
    fun drawGlBlendFrame(timeMs: Long, inputTexId: Int) {
        if (nativeBlend == 0L) return
        nativeAssBlendDrawFrame(nativeBlend, timeMs, inputTexId)
    }

    /**
     * Called BACK from the native EFFECTS_ATLAS worker thread (not the GL thread) to compute one
     * subtitle frame. Runs under [lock] exactly like [renderFrame], so it is safe with respect to
     * concurrent track mutation ([AssTrack.readChunk]) and [release] — this is what lets the native
     * worker thread safely touch libass state without caching a raw pointer across calls.
     *
     * Not called from anywhere in this module directly; invoked via JNI `CallBooleanMethod` from
     * `AssBlend.c`'s worker thread. Kept accessible (not private) for that reason, and protected from
     * R8/ProGuard stripping by this class's blanket `-keep` in consumer-rules.pro.
     */
    fun workerComputeBlendFrame(timeMs: Long): Boolean {
        lock.withLock {
            if (released || nativeRender == 0L || nativeBlend == 0L) return false
            val t = track ?: return false
            if (t.released || t.nativeAssTrack == 0L) return false
            return nativeAssBlendWorkerCompute(nativeBlend, nativeRender, t.nativeAssTrack, timeMs)
        }
    }

    /**
     * Releases the native GL blend state, stopping and joining the worker thread. Must be called on
     * the GL thread (mirrors [configureGlBlend]).
     *
     * Deliberately does NOT hold [lock] while joining the worker thread: [workerComputeBlendFrame]
     * needs that same lock to make progress and notice it should stop, so joining while holding it
     * would deadlock. Clearing [nativeBlend] to 0 under a brief lock first is what makes this safe —
     * any worker cycle that acquires the lock after that point sees a cleared handle and returns
     * immediately without touching native state, regardless of how long the actual join then takes.
     */
    fun releaseGlBlend() {
        val blend = lock.withLock {
            val current = nativeBlend
            nativeBlend = 0L
            current
        }
        if (blend != 0L) {
            nativeAssBlendRelease(blend)
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
