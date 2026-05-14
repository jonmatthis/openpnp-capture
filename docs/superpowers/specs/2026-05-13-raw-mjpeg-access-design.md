# Raw MJPEG Access for openpnp-capture

**Date:** 2026-05-13  
**Status:** Approved  
**Scope:** Fork (upstream-friendly, additive API)  
**Platforms:** Windows, Linux, macOS

## Motivation

High-performance multi-camera synchronized recording and streaming. By pulling raw JPEG bytes directly from the capture pipeline, we:

1. Eliminate per-frame decode overhead (no libjpeg/DirectShow/AVFoundation decode in the hot path)
2. Reduce payload size for streaming and storage (compressed JPEG vs uncompressed RGB24)
3. Defer decode to downstream consumers who actually need bitmaps (e.g., preview at lower framerate)

## Architecture: Approach A — Parallel Raw Buffer on Stream

Add a second buffer (`m_rawBuffer` + `m_rawFrameSize`) to the existing `Stream` base class. A mode flag set at stream-open time determines whether frames are stored raw or decoded.

---

## Public C API Additions

Four new functions in `include/openpnp-capture.h`:

```c
// Open a stream in raw mode — frames stored as compressed JPEG, not decoded.
// Returns -1 if the format is not MJPEG.
DLLPUBLIC CapStream Cap_openStreamRaw(CapContext ctx, CapDeviceID index, CapFormatID formatID);

// Copy the most recent raw JPEG frame into caller's buffer.
// Sets *outBytes to actual JPEG byte count.
// Returns CAPRESULT_ERR if buffer too small (outBytes still set to required size).
DLLPUBLIC CapResult Cap_captureFrameRaw(CapContext ctx, CapStream stream,
    void *jpegBufferPtr, uint32_t jpegBufferBytes, uint32_t *outBytes);

// Get the byte size of the current raw frame without copying.
DLLPUBLIC CapResult Cap_getFrameSize(CapContext ctx, CapStream stream, uint32_t *outBytes);

// Decode current raw JPEG into RGB on demand. Only valid on raw streams.
DLLPUBLIC CapResult Cap_decodeFrame(CapContext ctx, CapStream stream,
    void *RGBbufferPtr, uint32_t RGBbufferBytes);
```

Existing functions (`Cap_openStream`, `Cap_captureFrame`, `Cap_hasNewFrame`, `Cap_getStreamFrameCount`) remain unchanged. `Cap_hasNewFrame` works with both stream types. `Cap_captureFrame` on a raw stream returns `CAPRESULT_ERR`.

---

## Stream Base Class Changes

New members on `Stream` (`common/stream.h`):

```cpp
protected:
    bool        m_rawMode;              // immutable after open
    std::vector<uint8_t> m_rawBuffer;   // compressed JPEG storage
    uint32_t    m_rawFrameSize;         // actual bytes in current frame
    MJPEGHelper m_mjpegHelper;          // promoted from linux/ to common/
```

### Buffer Growth Strategy

```
Initial capacity: width * height * 3 / 4  (JPEG is ~25% of RGB size)

On frame arrival:
  if incomingBytes <= m_rawBuffer.size():
      memcpy, record length, done
  else:
      resize to incomingBytes * 2
      memcpy, record length
```

Reallocations are front-loaded (first 1-2 frames) then stabilize. Growth factor of 2x provides headroom for scene complexity changes.

### Initialization Flow

`Cap_openStreamRaw` in `common/context.cpp`:
1. Look up the format info for `formatID`, verify `fourcc == MJPG`
2. Create `PlatformStream` (same as `Cap_openStream`)
3. Set `stream->m_rawMode = true` **before** calling `stream->open()`
4. Call `stream->open(...)` — platform code checks `m_rawMode` to configure OS-level capture settings

This means `m_rawMode` is available inside `open()` when the platform configures DirectShow/V4L2/AVFoundation.

### New Base Class Methods

```cpp
// Called by platform code in raw mode
void Stream::submitRawBuffer(const uint8_t *ptr, size_t bytes);

// Called by Cap_captureFrameRaw
bool Stream::captureFrameRaw(uint8_t *outPtr, uint32_t outBytes, uint32_t *actualBytes);

// Called by Cap_getFrameSize — returns false if no frame received yet
bool Stream::getFrameSize(uint32_t *outBytes);

// Called by Cap_decodeFrame — holds mutex during decode (~1ms for 1080p)
bool Stream::decodeFrame(uint8_t *RGBbufferPtr, uint32_t RGBbufferBytes);
```

### Ring-Buffer Readiness

Single-frame semantics are entirely contained in `submitRawBuffer` and `captureFrameRaw`. Upgrading to a ring buffer later means replacing those two methods and the backing `m_rawBuffer` without touching the public API, platform code, or decode path.

---

## Platform-Specific Changes

### Linux (V4L2)

In `linux/platformstream.cpp`, the MJPG case in `threadSubmitBuffer`:

```cpp
case 0x47504A4D:    // MJPG
    if (m_rawMode) {
        submitRawBuffer((uint8_t*)ptr, bytes);
    } else {
        // existing decode path unchanged
        m_bufferMutex.lock();
        m_mjpegHelper.decompressFrame((uint8_t*)ptr, bytes, &m_frameBuffer[0], m_width, m_height);
        m_newFrame = true;
        m_frames++;
        m_bufferMutex.unlock();
    }
    break;
```

~10 lines changed. Raw JPEG bytes already in userspace via V4L2 mmap.

### Windows (DirectShow)

1. Sample Grabber media type changes based on mode:
   ```cpp
   mt.subtype = m_rawMode ? MEDIASUBTYPE_MJPG : MEDIASUBTYPE_RGB24;
   ```

2. `SampleCB` callback routes to appropriate submit:
   ```cpp
   if (m_stream->isRawMode()) {
       m_stream->submitRawBuffer(ptr, bytes);
   } else {
       m_stream->submitBuffer(ptr, bytes);
   }
   ```

When Sample Grabber accepts `MEDIASUBTYPE_MJPG`, DirectShow passes raw compressed bytes without inserting a decoder filter. The existing MJPG format fix (pin re-negotiation from commit `fde0ff1`) remains — it ensures the capture pin delivers MJPG at the correct resolution/framerate.

~30 lines changed.

### macOS (AVFoundation)

1. Video output settings change:
   ```objc
   if (m_rawMode) {
       output.videoSettings = nil;  // native format delivery
   } else {
       output.videoSettings = @{ (id)kCVPixelBufferPixelFormatTypeKey:
           @(kCVPixelFormatType_32ARGB) };
   }
   ```

2. Delegate callback extracts from `CMBlockBuffer` in raw mode:
   ```objc
   if (m_stream->isRawMode()) {
       CMBlockBufferRef blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
       size_t length = 0;
       char *dataPtr = NULL;
       CMBlockBufferGetDataPointer(blockBuffer, 0, NULL, &length, &dataPtr);
       m_stream->submitRawBuffer((uint8_t*)dataPtr, length);
   } else {
       // existing ARGB -> RGB path unchanged
   }
   ```

~40 lines changed.

---

## Decode-on-Demand

`MJPEGHelper` (currently `linux/mjpeghelper.h`) promoted to `common/mjpeghelper.h`. It wraps libjpeg-turbo (`tjInitDecompress` / `tjDecompress2` / `tjDestroy`).

```cpp
bool Stream::decodeFrame(uint8_t *RGBbufferPtr, uint32_t RGBbufferBytes)
{
    if (!m_rawMode || m_rawFrameSize == 0) return false;
    uint32_t needed = m_width * m_height * 3;
    if (RGBbufferBytes < needed) return false;

    m_bufferMutex.lock();
    bool ok = m_mjpegHelper.decompressFrame(
        &m_rawBuffer[0], m_rawFrameSize,
        RGBbufferPtr, m_width, m_height);
    m_bufferMutex.unlock();
    return ok;
}
```

**Thread safety note:** `decodeFrame` holds `m_bufferMutex` during decompression (~1ms for 1080p JPEG). This means a capture callback arriving during decode will briefly block. With single-frame semantics this is acceptable — worst case the callback overwrites immediately after decode releases the lock. For your use case (recording raw, decoding only for occasional preview), `decodeFrame` is called infrequently enough that contention is negligible.

**Build system:** libjpeg-turbo already vendored/fetched by CMake. Windows and macOS link targets updated to include it.

---

## Format Restriction

Raw mode is **MJPEG-only**. `Cap_openStreamRaw` checks the fourcc of the requested format:
- If MJPEG (`0x47504A4D` / `MJPG`): proceed
- Otherwise: return `-1` (invalid stream) with `LOG(LOG_ERR, ...)`

Callers should enumerate formats with `Cap_getFormatInfo`, check `fourcc`, and only request raw mode for MJPEG formats.

---

## Logging

Every decision point gets a log statement:

| Level | Events |
|-------|--------|
| `LOG_INFO` | Stream opened in raw mode, initial buffer capacity, platform config choices, first frame received |
| `LOG_WARNING` | Buffer growth events, caller buffer too small |
| `LOG_ERR` | Format not MJPEG, decode failures, invalid operations (e.g., `Cap_captureFrame` on raw stream) |
| `LOG_DEBUG` | Per-frame: bytes stored, bytes delivered |

Platform-specific one-time logs at setup:
- Windows: `"DirectShow: SampleGrabber media type set to MEDIASUBTYPE_MJPG (raw mode)"`
- Linux: `"V4L2: raw mode active, skipping MJPEG decode in threadSubmitBuffer"`
- macOS: `"AVFoundation: videoSettings=nil (native format delivery for raw mode)"`

Buffer lifecycle:
- `"Raw buffer: first frame received, %u bytes (capacity %u)"`
- `"Raw buffer growing: frame %u bytes exceeds capacity %u, resizing to %u"`

Use `Cap_setLogLevel(LOG_DEBUG)` during development to see per-frame flow; `LOG_INFO` in production for lifecycle events only.

---

## Error Handling

| Scenario | Behavior |
|----------|----------|
| `Cap_openStreamRaw` with non-MJPEG format | Returns -1, logs error |
| `Cap_captureFrame` on a raw stream | Returns `CAPRESULT_ERR`, logs guidance |
| `Cap_captureFrameRaw` on a non-raw stream | Returns `CAPRESULT_ERR` |
| Caller buffer too small for `Cap_captureFrameRaw` | Returns `CAPRESULT_ERR`, sets `*outBytes` to required size |
| `Cap_getFrameSize` before first frame received | Returns `CAPRESULT_ERR`, sets `*outBytes = 0` |
| `Cap_decodeFrame` when no frame available | Returns `CAPRESULT_ERR` |
| libjpeg-turbo decode failure | Returns `CAPRESULT_ERR`, logs error with frame size |

---

## Files Modified

| File | Change |
|------|--------|
| `include/openpnp-capture.h` | 4 new function declarations |
| `common/stream.h` | New members + method declarations |
| `common/stream.cpp` | `submitRawBuffer`, `captureFrameRaw`, `getFrameSize`, `decodeFrame` |
| `common/mjpeghelper.h` | Moved from `linux/mjpeghelper.h` |
| `common/mjpeghelper.cpp` | Moved from `linux/mjpeghelper.cpp` |
| `common/context.cpp` | Route new API calls to Stream methods |
| `linux/platformstream.cpp` | if/else in MJPG case of `threadSubmitBuffer` |
| `win/platformstream.cpp` | SampleGrabber media type + callback routing |
| `win/platformstream.h` | `isRawMode()` accessor |
| `mac/platformstream.mm` | videoSettings + delegate callback routing |
| `mac/platformstream.h` | `isRawMode()` accessor |
| `CMakeLists.txt` | Link libjpeg-turbo on all platforms, move mjpeghelper to common sources |

---

## Out of Scope (Future Work)

- Ring buffer (designed for, not implemented)
- Raw access for non-MJPEG formats (YUV, NV12)
- Timestamp/metadata per frame
- Zero-copy API (returning pointer into internal buffer instead of memcpy)
