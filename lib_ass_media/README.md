# ASS Media
A media3 extend library for libass.

App use media3 can use this module to add ass for your player.

## Feature
There are multiple ways to render ass subtitle.
Which is defined in `AssRenderType`.

| Type | Feature | Anim | HDR/DV | Block render/UI |
| :----: | :----: | :----: | :----: | :----: |
| CUES | SubtitleView & Cue | ❌ | ✅ | ❌ |
| EFFECTS_CANVAS | Effect | ✅ | ❓ | ✅ |
| EFFECTS_OPEN_GL | Effect | ✅ | ❓ | ✅ |
| EFFECTS_ATLAS | Effect (GlEffect, native atlas) | ✅ | ❓ | ✅ |
| OVERLAY_CANVAS | Overlay | ✅ | ✅ | ✅ |
| OVERLAY_OPEN_GL | Overlay | ✅ | ✅ | ❌ |

* [OverlayShaderProgram does not support HDR colors yet](https://github.com/androidx/media/issues/723)
* [Why does TextOverLay support hdr, but Bitmap not support?](https://github.com/androidx/media/issues/2383)

### 1. CUES
The ass/ssa subtitle will be parsed and transcode to bytes, and decode to bitmap when render.

This type not support dynamic feature, because all subtitle and it time is static.

But since the subtitle is transcode, it will not cost too much time when render. All work is done in parse thread.

### 2. EFFECTS_CANVAS
The ass/ssa subtitle will be cal and render at runtime use media3 effect feature, and this will support all dynamic features.

And this need to create a screen size offscreen bitmap to render the libass bitmap pieces.

But when the dynamic feature is too complex, and libass will cost too much time to cal, the render will be blocked.

### 3. EFFECTS_OPEN_GL
Just like `EFFECTS_CANVAS`, but use OpenGL to render. and the offscreen tex is create to render the bitmap pieces.

Due to test, the `EFFECTS_OPEN_GL` will save 1/3 time when render.

### 4. EFFECTS_ATLAS
The fastest render type. Uses media3's low-level `GlEffect`/`GlShaderProgram` API directly (not the
`OverlayEffect`/`TextureOverlay` wrapper `EFFECTS_OPEN_GL` uses), and every part of the blending
(libass render, atlas packing, atlas upload, video blit, subtitle draw) is done natively in a
single JNI call per frame.

Instead of creating and destroying one GL texture per glyph piece every frame (what every other
render type does today), glyph pieces are shelf-packed into a single persistent, reused GPU
texture atlas. This reduces per-frame subtitle rendering to at most two draw calls: one full-frame
video blit and one batched draw covering every currently visible glyph piece.

`ass_render_frame` runs on a dedicated native worker thread, not on the GL thread `drawFrame` is
called on: the GL thread never calls into libass directly, it hands the worker the latest requested
time and bounded-waits up to `AssHandlerConfig.blendWorkerWaitMs` (default 16ms) for that exact
request to finish before uploading/drawing whatever atlas state was published. Frames that finish
within the budget stay frame-accurate; a frame that doesn't falls back to stale content for that
frame (logged as `TIMED OUT` under logcat tag `AssBlend`) rather than stalling video delivery.
Real-world `ass_render_frame` cost varies a lot with content — profile with that log tag and tune
`blendWorkerWaitMs` against your video's actual frame period; lower-frame-rate content has much more
headroom to raise it for consistently frame-accurate subtitles.

Same HDR/DV limitation as `EFFECTS_OPEN_GL` (a plain 2D GL texture pipeline, no HDR color
handling).

Like the other `EFFECTS_*` types, subtitles are rasterized at the *video's own decode resolution*,
not the display surface's — this GL stage's output must match its input (media3's effect pipeline
reports this stage's output size to ExoPlayer as the new video size, which `PlayerView` uses to lay
out its surface, so reporting anything other than the true video size here creates a feedback loop
with the surface's own size). For low-resolution source video shown on a much higher-resolution
display, subtitles will look softer than `OVERLAY_CANVAS`/`OVERLAY_OPEN_GL` (which render at surface
resolution as a separate layer, independent of the video). Use `OVERLAY_CANVAS`/`OVERLAY_OPEN_GL`
instead if surface-resolution-sharp subtitles matter more than `EFFECTS_ATLAS`'s blending speed.

### 5. OVERLAY_CANVAS
The ass/ssa subtitle will be cal at runtime, and add a `Overlay` widget in `SubtitleView` to render subtitle.

The `libass` render result will copy to bitmap, and draw in `Canvas`.

It will block UI thread when rendering.

### 6. OVERLAY_OPEN_GL
Just like `OVERLAY_CANVAS`, but the `libass` render result will pass to `OpenGL` texture, and avoid create tmp bitmap.

It will save half memory than `OVERLAY_CANVAS`.

And the `libass` render and `OpenGL` draw on another separate thread, it will not block the UI thread like `OVERLAY_CANVAS`.


## How to use
1. Add MavenCenter to your project
    ```
    allprojects {
        repositories {
            mavenCentral()
        }
    }
    ```
2. Add dependency.
    ```
   implementation "io.github.peerless2012:ass-media:x.x.x"
    ```
3. Use libass-media in java/kotlin
    ```
    player = ExoPlayer.Builder(this)
    .buildWithAssSupport(
        this,
        AssRenderType.OPEN_GL
    )
    ```
4. Add external subtitles.
   ```
   val enConfig = MediaItem.SubtitleConfiguration
         .Builder(Uri.parse("http://192.168.0.254:80/files/f-en.ass"))
         .setMimeType(MimeTypes.TEXT_SSA)
         .setLanguage("en")
         .setLabel("External ass en")
         .setId("129")
         .setSelectionFlags(C.SELECTION_FLAG_DEFAULT)
         .build()
   val jpConfig = MediaItem.SubtitleConfiguration
         .Builder(Uri.parse("http://192.168.0.254:80/files/f-jp.ass"))
         .setMimeType(MimeTypes.TEXT_SSA)
         .setLanguage("jp")
         .setLabel("External ass jp")
         .setId("130")
         .build()
   val zhConfig = MediaItem.SubtitleConfiguration
         .Builder(Uri.parse("http://192.168.0.254:80/files/f-zh.ass"))
         .setMimeType(MimeTypes.TEXT_SSA)
         .setLanguage("zh")
         .setLabel("External ass zh")
         .setId("131")
         .build()
   val mediaItem = MediaItem.Builder()
         .setUri(url)
         .setSubtitleConfigurations(ImmutableList.of(enConfig, jpConfig, zhConfig))
   ```
   NOTE: Make sure the `id` is set and different from media self track size. Recommend bigger than 128 or more bigger.

## Configuration

`AssHandlerConfig` provides options to tune subtitle rendering behavior. Pass it to `buildWithAssSupport`:

```kotlin
player = ExoPlayer.Builder(this)
    .buildWithAssSupport(
        context = this,
        renderType = AssRenderType.OVERLAY_OPEN_GL,
        config = AssHandlerConfig(
            maxRenderPixels = 1920 * 1080
        )
    )
```

### Parameters

| Parameter | Default | Description |
| :----: | :----: | :---- |
| `glyphSize` | 10000 | Maximum number of glyph cache entries in libass |
| `cacheSize` | 128 | Maximum bitmap cache size in MB for libass |
| `maxRenderPixels` | 0 | Maximum pixel count for subtitle rendering (0 = no limit) |

### Render Downscaling (`maxRenderPixels`)

On high-resolution devices (e.g., 4K TVs), rendering subtitles at full resolution can be very CPU and memory intensive, especially for complex ASS animations. `maxRenderPixels` limits the resolution at which libass renders subtitles.

**How it works:**

```
Target frame size: 3840 x 2160 = 8,294,400 pixels
maxRenderPixels:   1920 x 1080 = 2,073,600

scale = sqrt(2,073,600 / 8,294,400) = 0.5
Actual render size: 1920 x 1080
```

1. libass renders subtitle bitmaps at the downscaled size (e.g., 1080p)
2. The rendered subtitle images are then scaled up to the actual surface/video size during display
3. GPU hardware handles the upscaling, which is nearly free

**Recommended values:**

| Value | Equivalent | Use case |
| :---- | :---- | :---- |
| `0` | No limit | Default, render at full resolution |
| `2_073_600` | 1080p | 4K devices with normal CPU |
| `3_686_400` | 1440p | 4K devices with strong CPU |
| `921_600` | 720p | Low-end devices or complex ASS animations |

**Mode-specific behavior:**

- **OVERLAY_OPEN_GL**: Renders at reduced size, scales `glViewport` coordinates to surface size
- **OVERLAY_CANVAS**: Renders at reduced size, uses `drawBitmap` with scaled `RectF`
- **EFFECTS_OPEN_GL**: Creates smaller FBO, uses `getVertexTransformation` to scale up in the OverlayEffect pipeline
- **EFFECTS_ATLAS**: Renders at reduced size; subtitle quad clip-space coordinates are computed against the render size, then upscaled for free since the video blit already fills the full output size
- **EFFECTS_CANVAS**: Renders at reduced size, scales canvas draw coordinates
- **CUES**: Not affected (pre-renders all subtitles at parse time)
