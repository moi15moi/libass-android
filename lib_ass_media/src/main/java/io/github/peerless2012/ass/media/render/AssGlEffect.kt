package io.github.peerless2012.ass.media.render

import android.content.Context
import androidx.annotation.OptIn
import androidx.media3.common.util.UnstableApi
import androidx.media3.effect.GlEffect
import androidx.media3.effect.GlShaderProgram
import io.github.peerless2012.ass.AssRender

/**
 * A [GlEffect] that draws ASS subtitles directly onto each decoded video frame's own texture, in
 * place, via [AssGlShaderProgram] — instead of going through the `OverlayEffect`/`TextureOverlay`
 * convenience wrapper, which would require a separate output texture. Used by
 * [AssRenderType.EFFECTS_ATLAS][io.github.peerless2012.ass.media.type.AssRenderType.EFFECTS_ATLAS].
 */
@OptIn(UnstableApi::class)
class AssGlEffect(
    private val render: AssRender
) : GlEffect {

    override fun toGlShaderProgram(context: Context, useHdr: Boolean): GlShaderProgram {
        return AssGlShaderProgram(render)
    }
}
