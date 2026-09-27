package io.github.peerless2012.ass.media.render

import android.content.Context
import androidx.annotation.OptIn
import androidx.media3.common.util.UnstableApi
import androidx.media3.effect.GlEffect
import androidx.media3.effect.GlShaderProgram
import io.github.peerless2012.ass.AssRender
import io.github.peerless2012.ass.media.AssHandler

/**
 * A [GlEffect] that bakes ASS subtitles directly into ExoPlayer's video effects pipeline via
 * [AssGlShaderProgram], instead of going through the `OverlayEffect`/`TextureOverlay`
 * convenience wrapper. Used by [AssRenderType.EFFECTS_ATLAS][io.github.peerless2012.ass.media.type.AssRenderType.EFFECTS_ATLAS].
 */
@OptIn(UnstableApi::class)
class AssGlEffect(
    private val handler: AssHandler,
    private val render: AssRender
) : GlEffect {

    override fun toGlShaderProgram(context: Context, useHdr: Boolean): GlShaderProgram {
        return AssGlShaderProgram(handler, render)
    }
}
