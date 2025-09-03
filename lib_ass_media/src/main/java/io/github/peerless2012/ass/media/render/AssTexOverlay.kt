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

    private val vertexShaderCode = """
            attribute vec4 a_Position;
            attribute vec2 a_TexCoord;
            varying vec2 v_TexCoord;
            void main() {
                gl_Position = a_Position;
                v_TexCoord = a_TexCoord.xy;
            }
        """.trimIndent()

    // alpha
    private val fragmentShaderCode = """
            precision mediump float;
            varying vec2 v_TexCoord;
            uniform sampler2D u_Texture;
            uniform vec4 u_Color;
            void main() {
                float alpha = texture2D(u_Texture, v_TexCoord).a;
                gl_FragColor = u_Color * alpha;

            }
        """.trimIndent()

    private val rectangleCoords = floatArrayOf(
        -1f,  1f,  // Top left
        1f,  1f,  // Top right
        -1f, -1f,  // Bottom left
        1f, -1f   // Bottom right
    )

    private val textureCoords = floatArrayOf(
        0f, 0f,  // Top left
        1f, 0f,  // Top right
        0f, 1f,  // Bottom left
        1f, 1f   // Bottom right
    )

    private val preFbo = IntArray(1)

    private val preViewPort = IntArray(4)

    private val preTex = IntArray(1)

    private val preAlign = IntArray(1)

    private var texId = 0

    private var texDirty = true

    private var texSize = Size.ZERO

    private var fboId = 0

    private lateinit var glProgram: GlProgram

    private var vertexBufferId = 0

    private var texCoordBufferId = 0

    private lateinit var executor: AssExecutor

    override fun getTextureId(presentationTimeUs: Long): Int {
        val timeUs = if (handler.videoTime >= 0) {
            handler.videoTime
        } else {
            presentationTimeUs
        }
        val texId = executor.renderFrame(timeUs)

        if (texId == null){
            this.texId = 0
        } else {
            this.texId = texId.toInt()
        }

        Log.d("SubtitleRenderer", "We got textid=$texId")

        return this.texId
    }

    override fun getTextureSize(presentationTimeUs: Long): Size {
        if (texId == null) {
            return Size.ZERO
        }
        return Size(executor.render.width, executor.render.height)
    }

    override fun configure(videoSize: Size) {
        super.configure(videoSize)
        this.texSize = videoSize
        executor = AssExecutor(render)
        render.setFrameSize(videoSize.width, videoSize.height)
        texId = GlUtil.createTexture(videoSize.width, videoSize.height, false)
        fboId = GlUtil.createFboForTexture(texId)
        glProgram = GlProgram(vertexShaderCode, fragmentShaderCode)
        GlUtil.checkGlError()

        val vertexBuffer = ByteBuffer.allocateDirect(rectangleCoords.size * 4)
            .order(ByteOrder.nativeOrder())
            .asFloatBuffer()
            .put(rectangleCoords)
        vertexBuffer.position(0)

        val texCordBuffer = ByteBuffer.allocateDirect(textureCoords.size * 4)
            .order(ByteOrder.nativeOrder())
            .asFloatBuffer()
            .put(textureCoords)
        texCordBuffer.position(0)

        val buffers = IntArray(2)
        GLES20.glGenBuffers(2, buffers, 0)
        vertexBufferId = buffers[0]
        texCoordBufferId = buffers[1]
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, vertexBufferId)
        GLES20.glBufferData(GLES20.GL_ARRAY_BUFFER, rectangleCoords.size * 4, vertexBuffer, GLES20.GL_STATIC_DRAW)
        GlUtil.checkGlError()
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, texCoordBufferId)
        GLES20.glBufferData(GLES20.GL_ARRAY_BUFFER, textureCoords.size * 4, texCordBuffer, GLES20.GL_STATIC_DRAW)
        GlUtil.checkGlError()

        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, 0)
    }

    override fun release() {
        GlUtil.deleteFbo(fboId)
        GlUtil.deleteTexture(texId)
        GlUtil.deleteBuffer(vertexBufferId)
        GlUtil.deleteBuffer(texCoordBufferId)
        glProgram.delete()
        executor.shutdown()
        super.release()
    }
}