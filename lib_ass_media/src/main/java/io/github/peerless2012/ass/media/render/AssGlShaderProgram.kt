package io.github.peerless2012.ass.media.render

import android.util.Log
import androidx.annotation.OptIn
import androidx.media3.common.C
import androidx.media3.common.GlObjectsProvider
import androidx.media3.common.GlTextureInfo
import androidx.media3.common.VideoFrameProcessingException
import androidx.media3.common.util.GlUtil
import androidx.media3.common.util.UnstableApi
import androidx.media3.effect.GlShaderProgram
import io.github.peerless2012.ass.AssRender
import java.util.concurrent.Executor

/**
 * Draws ASS subtitles directly onto each decoded video frame's own texture, in place, and forwards
 * that same texture downstream unchanged — no separate output texture, no full-frame video copy,
 * and no GPU work at all on frames with no visible subtitle content.
 *
 * Implements the raw [GlShaderProgram] interface directly rather than the higher-level
 * `BaseGlShaderProgram` convenience wrapper (which [AssTexOverlay]'s `OverlayEffect`/
 * `TextureOverlay` path builds on) specifically to get this in-place behavior:
 * `BaseGlShaderProgram` always hands you a fresh, already-cleared *output* framebuffer you must
 * fill completely, which would force a redundant full-frame blit of the video on every single
 * frame just to reproduce it unchanged — an earlier version of this class did exactly that.
 *
 * All actual GL work (libass render, atlas packing/upload, the batched subtitle draw) happens
 * natively in a single call per frame — see [AssRender.drawOverlayFrame] and `AssOverlay.c`'s
 * `nativeAssOverlayDraw`. `ass_render_frame` runs synchronously, inline, on this call: always
 * frame-accurate by construction, at the cost of being able to stall video delivery if a subtitle
 * frame is pathologically slow to render (see `AssOverlay.c`'s file header comment for the full
 * tradeoff against the worker-thread design this replaced).
 */
@OptIn(UnstableApi::class)
class AssGlShaderProgram(
    private val render: AssRender
) : GlShaderProgram {

    private var inputListener: GlShaderProgram.InputListener = object : GlShaderProgram.InputListener {}
    private var outputListener: GlShaderProgram.OutputListener = object : GlShaderProgram.OutputListener {}
    private var errorListener = GlShaderProgram.ErrorListener {}
    private var errorExecutor = Executor { it.run() }

    private var frameInFlight = false

    /** Only populated when upstream doesn't already hand us an FBO for a given texture (fboId
     * unset) — keyed by texId so a texture media3's pool reuses doesn't leak a new FBO every time. */
    private val fallbackFbos = HashMap<Int, Int>()

    override fun setInputListener(listener: GlShaderProgram.InputListener) {
        inputListener = listener
        if (!frameInFlight) listener.onReadyToAcceptInputFrame()
    }

    override fun setOutputListener(listener: GlShaderProgram.OutputListener) {
        outputListener = listener
    }

    override fun setErrorListener(executor: Executor, listener: GlShaderProgram.ErrorListener) {
        errorExecutor = executor
        errorListener = listener
    }

    override fun queueInputFrame(
        glObjectsProvider: GlObjectsProvider,
        inputTexture: GlTextureInfo,
        presentationTimeUs: Long
    ) {
        try {
            val fbo = if (inputTexture.fboId != C.INDEX_UNSET) {
                inputTexture.fboId
            } else {
                fallbackFbos.getOrPut(inputTexture.texId) {
                    GlUtil.createFboForTexture(inputTexture.texId)
                }
            }
            val startNs = System.nanoTime()
            render.drawOverlayFrame(
                fbo,
                inputTexture.width,
                inputTexture.height,
                presentationTimeUs / 1000
            )
            val elapsedMs = (System.nanoTime() - startNs) / 1_000_000
            if (elapsedMs >= SLOW_FRAME_LOG_THRESHOLD_MS) {
                Log.w(TAG, "drawOverlayFrame took ${elapsedMs}ms for presentationTimeUs=$presentationTimeUs (see logcat tag AssOverlay for native-side timing breakdown)")
            }
        } catch (e: Exception) {
            errorExecutor.execute {
                errorListener.onError(VideoFrameProcessingException.from(e, presentationTimeUs))
            }
        }
        // Mark in-flight *before* forwarding: downstream may release the frame synchronously.
        frameInFlight = true
        outputListener.onOutputFrameAvailable(inputTexture, presentationTimeUs)
    }

    override fun releaseOutputFrame(outputTexture: GlTextureInfo) {
        frameInFlight = false
        inputListener.onInputFrameProcessed(outputTexture) // hand the texture back to its owner
        inputListener.onReadyToAcceptInputFrame()
    }

    override fun signalEndOfCurrentInputStream() {
        outputListener.onCurrentOutputStreamEnded()
    }

    override fun flush() {
        frameInFlight = false // upstream reclaims its own textures on flush
        inputListener.onFlush()
        inputListener.onReadyToAcceptInputFrame()
    }

    override fun release() {
        fallbackFbos.values.forEach { GlUtil.deleteFbo(it) }
        fallbackFbos.clear()
        render.releaseOverlayGl()
    }

    private companion object {
        private const val TAG = "AssGlShaderProgram"

        /** Logged when a single [queueInputFrame] call (Kotlin JNI call boundary included) takes
         * at least this long — a coarse "was this frame slow" signal on top of AssOverlay.c's more
         * detailed native-side (AssOverlay tag) timing. */
        private const val SLOW_FRAME_LOG_THRESHOLD_MS = 4
    }
}
