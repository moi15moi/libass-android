package io.github.peerless2012.ass.media.render

import android.util.Log
import androidx.annotation.OptIn
import androidx.media3.common.util.Size
import androidx.media3.common.util.UnstableApi
import androidx.media3.effect.BaseGlShaderProgram
import io.github.peerless2012.ass.AssRender
import io.github.peerless2012.ass.media.AssHandler

/**
 * A [BaseGlShaderProgram] that blends ASS subtitles directly into the decoded video frame.
 *
 * Unlike [AssTexOverlay] (which implements the higher-level `TextureOverlay`/`OverlayEffect`
 * abstraction), this class talks to the raw `GlEffect`/`GlShaderProgram` API: media3 hands it the
 * input video texture and an already-bound, already-cleared output framebuffer, and it is
 * responsible for producing the *entire* output frame itself.
 *
 * All actual GL work (subtitle rendering, atlas packing/upload, the video blit, and the batched
 * subtitle draw) happens natively in a single call per frame — see [AssRender.drawGlBlendFrame]
 * and `AssBlend.c`'s `nativeAssBlendDrawFrame`. This class is intentionally just a thin shim
 * satisfying the [BaseGlShaderProgram] contract.
 */
@OptIn(UnstableApi::class)
class AssGlShaderProgram(
    private val handler: AssHandler,
    private val render: AssRender
) : BaseGlShaderProgram(/* useHighPrecisionColorComponents= */ false, /* texturePoolCapacity= */ 1) {

    override fun configure(inputWidth: Int, inputHeight: Int): Size {
        // Output must match input (inputWidth x inputHeight, the video's own decode resolution), not
        // some larger "sharper" size: this stage's returned Size becomes the GlEffect pipeline's
        // reported output size, which flows into ExoPlayer's own onVideoSizeChanged (see
        // PlaybackVideoGraphWrapper.onOutputSizeChanged -> DefaultVideoSink -> the public
        // Player.Listener callback) - and PlayerView's AspectRatioFrameLayout uses THAT to lay out
        // the surface, i.e. handler.surfaceSize. Rendering at surfaceSize and reporting surfaceSize
        // back as the output size is a feedback loop: it visibly zooms/distorts the video, and
        // surfaceSize may not even be aspect-correct yet the first time configure() runs (it can fire
        // before AspectRatioFrameLayout has done its real, decoder-Format-driven layout pass). Subtitle
        // sharpness here is therefore capped by the video's own decode resolution - use
        // OVERLAY_CANVAS/OVERLAY_OPEN_GL instead if you need sharpness independent of video resolution
        // (they render as a separate layer on top of the surface and never touch the reported video
        // size at all).
        val renderSize = handler.computeRenderSize(inputWidth, inputHeight)

        // Storage size is the video's own decode dimensions: libass uses it to map the ASS script's
        // PlayResX/PlayResY-relative coordinates onto pixels. Explicit here (not just relying on
        // AssHandler's earlier call) so this function is correct standalone regardless of call
        // ordering elsewhere.
        render.setStorageSize(inputWidth, inputHeight)
        render.setFrameSize(renderSize.width, renderSize.height)
        render.configureGlBlend(renderSize.width, renderSize.height, handler.config.blendWorkerWaitMs)
        return Size(inputWidth, inputHeight)
    }

    override fun drawFrame(inputTexId: Int, presentationTimeUs: Long) {
        // presentationTimeUs here is the exact, authoritative timestamp for the frame this call is
        // compositing (handed to us synchronously by the same GlEffect pipeline call) - unlike
        // handler.videoTime, which is set asynchronously from ExoPlayer's own polling loop and is
        // not guaranteed to correspond to this specific frame.
        val startNs = System.nanoTime()
        render.drawGlBlendFrame(presentationTimeUs / 1000, inputTexId)
        val elapsedMs = (System.nanoTime() - startNs) / 1_000_000
        if (elapsedMs >= SLOW_FRAME_LOG_THRESHOLD_MS) {
            Log.w(TAG, "drawGlBlendFrame took ${elapsedMs}ms for presentationTimeUs=$presentationTimeUs (see logcat tag AssBlend for native-side timing breakdown)")
        }
    }

    override fun release() {
        render.releaseGlBlend()
        super.release()
    }

    private companion object {
        private const val TAG = "AssGlShaderProgram"

        /** Logged when a single [drawFrame] call (Kotlin JNI call boundary included) takes at
         * least this long — a coarse "was this frame slow" signal on top of AssBlend.c's more
         * detailed native-side (AssBlend tag) timing. */
        private const val SLOW_FRAME_LOG_THRESHOLD_MS = 4
    }
}
