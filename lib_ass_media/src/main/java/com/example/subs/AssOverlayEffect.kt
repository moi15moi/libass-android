package com.example.subs // JNI symbol names in ass_gl_overlay.cpp depend on this package

import android.content.Context
import android.util.Log
import androidx.annotation.OptIn
import androidx.media3.common.C
import androidx.media3.common.GlObjectsProvider
import androidx.media3.common.GlTextureInfo
import androidx.media3.common.VideoFrameProcessingException
import androidx.media3.common.util.GlUtil
import androidx.media3.common.util.UnstableApi
import androidx.media3.effect.GlEffect
import androidx.media3.effect.GlShaderProgram
import java.util.concurrent.Executor

/**
 * Burns libass subtitles into each video frame.
 *
 *   player.setVideoEffects(listOf(AssEffect(nativeHandle)))
 *
 * nativeHandle is a pointer to the AssGl struct from ass_gl_overlay.cpp.
 */
@OptIn(UnstableApi::class)
class AssEffect(private val nativeHandle: Long) : GlEffect {
    override fun toGlShaderProgram(context: Context, useHdr: Boolean): GlShaderProgram {
        if (useHdr) throw VideoFrameProcessingException("AssEffect only supports SDR input")
        return AssOverlayProgram(nativeHandle)
    }
}

/**
 * In-place pass-through: draws the subtitles straight into the incoming texture and
 * forwards that same texture downstream. No output texture, no full-frame copy, and
 * no GPU work at all on frames without subtitles.
 */
@OptIn(UnstableApi::class)
class AssOverlayProgram(private val handle: Long) : GlShaderProgram {

    // Not part of the original shared reference file: the file's own setup comment
    // ("auto *c = new AssGl{renderer, track}; ... pass reinterpret_cast<jlong>(c) to AssEffect(...)")
    // is pseudocode, not a callable entry point, so this is the minimal glue needed to actually
    // create/destroy an AssGl handle from Kotlin. See ass_gl_overlay.cpp's matching addition.
    companion object {
        @JvmStatic
        external fun nativeCreate(rendererPtr: Long, trackPtr: Long): Long

        @JvmStatic
        external fun nativeDestroy(handle: Long)

        // Also not part of the original reference file - see the timing log in queueInputFrame.
        private const val TAG = "AssOverlayProgram"
        private const val SLOW_FRAME_LOG_THRESHOLD_MS = 4 // matches AssGlShaderProgram's own threshold
    }

    private var inputListener: GlShaderProgram.InputListener =
        object : GlShaderProgram.InputListener {}
    private var outputListener: GlShaderProgram.OutputListener =
        object : GlShaderProgram.OutputListener {}
    private var errorListener = GlShaderProgram.ErrorListener {}
    private var errorExecutor = Executor { it.run() }

    private var frameInFlight = false
    private val fallbackFbos = HashMap<Int, Int>() // texId -> fbo, only if upstream gave no FBO

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
        presentationTimeUs: Long,
    ) {
        try {
            val fbo =
                if (inputTexture.fboId != C.INDEX_UNSET) inputTexture.fboId
                else fallbackFbos.getOrPut(inputTexture.texId) {
                    GlUtil.createFboForTexture(inputTexture.texId)
                }
            // Not part of the original shared reference file: coarse per-call timing, mirroring
            // AssGlShaderProgram's equivalent log, for A/B-comparing against EFFECTS_ATLAS. Native-
            // side detail (ass_render_frame / uploadAtlas / buildVertices / drawSubtitles) is under
            // logcat tag "AssGl".
            val startNs = System.nanoTime()
            nativeDraw(handle, fbo, inputTexture.width, inputTexture.height, toAssTimeMs(presentationTimeUs))
            val elapsedMs = (System.nanoTime() - startNs) / 1_000_000
            if (elapsedMs >= SLOW_FRAME_LOG_THRESHOLD_MS) {
                Log.w(TAG, "nativeDraw took ${elapsedMs}ms for presentationTimeUs=$presentationTimeUs (see logcat tag AssGl for native-side timing breakdown)")
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
        nativeReleaseGl(handle)
        nativeDestroy(handle) // not in the original reference: frees the AssGl struct itself
    }

    /**
     * Maps the effect-chain timestamp to the subtitle timeline (libass wants milliseconds).
     * Log a few values first: depending on the Media3 version they can include ExoPlayer's
     * internal renderer offset (values around 1e12 us). If so, subtract it here.
     */
    private fun toAssTimeMs(presentationTimeUs: Long): Long = presentationTimeUs / 1000

    private external fun nativeDraw(handle: Long, fbo: Int, width: Int, height: Int, ptsMs: Long)
    private external fun nativeReleaseGl(handle: Long)
}
