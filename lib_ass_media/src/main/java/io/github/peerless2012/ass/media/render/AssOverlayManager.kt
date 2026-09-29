package io.github.peerless2012.ass.media.render

import androidx.annotation.OptIn
import androidx.media3.common.Effect
import androidx.media3.common.util.UnstableApi
import androidx.media3.effect.OverlayEffect
import androidx.media3.exoplayer.ExoPlayer
import com.example.subs.AssEffect
import com.example.subs.AssOverlayProgram
import io.github.peerless2012.ass.AssRender
import io.github.peerless2012.ass.media.AssHandler
import io.github.peerless2012.ass.media.type.AssRenderType

@OptIn(UnstableApi::class)
class AssOverlayManager(
    private val handler: AssHandler,
    private val player: ExoPlayer,
    private val renderType: AssRenderType
) {
    private var currentRenderer : AssRender? = null

    init {
        // ExoPlayer documentation states that this needs to be called before .prepare()
        player.setVideoEffects(listOf())
    }

    fun enable(renderer: AssRender) {
        if (renderer == currentRenderer) return
        this.currentRenderer = renderer
        val effect: Effect = when (renderType) {
            AssRenderType.EFFECTS_ATLAS -> AssGlEffect(handler, renderer)
            // A/B comparison effect (see AssRenderType.EFFECTS_ATLAS_CPP's doc): captures whatever
            // track is current right now, via ass_gl_overlay.cpp's own nativeCreate - it will not
            // pick up later track changes the way AssGlEffect does.
            AssRenderType.EFFECTS_ATLAS_CPP -> {
                val track = handler.track
                val handle = if (track != null && track.nativeAssTrack != 0L) {
                    AssOverlayProgram.nativeCreate(renderer.nativeRenderPtr, track.nativeAssTrack)
                } else {
                    0L
                }
                AssEffect(handle)
            }
            AssRenderType.EFFECTS_OPEN_GL -> OverlayEffect(listOf(AssTexOverlay(handler, renderer)))
            else -> OverlayEffect(listOf(AssCanvasOverlay(handler, renderer)))
        }
        player.setVideoEffects(listOf(effect))
    }

    fun disable() {
        if (currentRenderer == null) return

        currentRenderer = null
        player.setVideoEffects(listOf())
    }

}
