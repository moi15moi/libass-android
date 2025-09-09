package io.github.peerless2012.ass.media.render

import android.opengl.GLES20
import android.util.Log
import androidx.annotation.OptIn
import androidx.media3.common.util.GlProgram
import androidx.media3.common.util.GlUtil
import androidx.media3.common.util.Size
import androidx.media3.common.util.UnstableApi
import androidx.media3.effect.TextureOverlay
import io.github.peerless2012.ass.AssRender
import io.github.peerless2012.ass.media.AssHandler
import io.github.peerless2012.ass.media.executor.AssExecutor
import java.nio.ByteBuffer
import java.nio.ByteOrder

@OptIn(UnstableApi::class)
class AssTexOverlay(private val handler: AssHandler, private val render: AssRender) : TextureOverlay() {
    private var texId = 0
    private var defaultTexId = 0
    private var texSize = Size.ZERO
    private var context: Long = 0

    private lateinit var executor: AssExecutor

    override fun getTextureId(presentationTimeUs: Long): Int {
        val timeUs = if (handler.videoTime >= 0) {
            handler.videoTime
        } else {
            presentationTimeUs
        }
        render.setFrameSize(executor.render.width, executor.render.height)

        val texId = executor.renderFrame(context, timeUs)

        if (texId == null){
            this.texId = defaultTexId
        } else {
            this.texId = texId.toInt()
        }

        Log.d("SubtitleRenderer", "We got textid=$texId")

        return this.texId
    }

    override fun getTextureSize(presentationTimeUs: Long): Size {
        if (texId == 0) {
            return Size.ZERO
        }
        return Size(executor.render.width, executor.render.height)
    }

    override fun configure(videoSize: Size) {
        super.configure(videoSize)
        this.texSize = videoSize
        executor = AssExecutor(render)
        defaultTexId = GlUtil.createTexture(0, 0, false)
        context = render.initializeLibplacebo()
    }

    override fun release() {
        executor.shutdown()
        super.release()
        render.uninitializeLibplacebo(context)
    }
}