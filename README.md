    ![OpenPNP Logo](https://raw.githubusercontent.com/openpnp/openpnp-logo/develop/logo_small.png)

# openpnp-capture
OpenPnP Capture is a cross platform video capture library for C with a focus on machine vision. Its goals are:

* Native camera access on Windows, Linux and Mac. Implemented with DirectShow, V4L2 and AVFoundation respectively.
* Named device enumeration.
* Strong, repeatable, unique IDs.
* Format control with support for at least YUV and MJPEG. Compressed formats such as MJPEG allow the use of multiple USB cameras on a hub or root controller.
* Auto and manual exposure control.
* Auto and manual focus control.
* Simple, common C wrapper for the listed APIs.


# Features
| Feature                  |  Windows   |    macOS     |     Linux      |
| ------------------------ |:----------:|:------------:|:--------------:|
| Platform API             | DirectShow | AVFoundation |      V4L2      |
| Named Device Enumeration |    Yes     |     Yes      |      Yes       |
| Capturing                |    Yes     |     Yes      |      Yes       |
| MJPEG formats            |    Yes     |  Yes (dmb1)  |      Yes       |
| Raw MJPEG capture        |    Yes     |     Yes      |      Yes       |
| YUV formats              |    Yes     |     Yes      | Yes, YUYV/YUV2 |
| Exposure control         |    Yes     |     Yes      |      Yes       |
| Focus control            |    Yes     |     Yes      |      Yes       |
| Zoom control             |    Yes     |     Yes      |      Yes       |
| Gain control             |    Yes     |     Yes      |      Yes       |
| White balance control    |    Yes     |     Yes      |      Yes       |
| Common C API             |    Yes     |     Yes      |      Yes       |
| Framerate control        |     No     |      No      |       No       |
| Re-Enumeration           |     No     |      No      |       No       |


# Getting Started
Packages and binaries are available in [releases](https://github.com/openpnp/openpnp-capture/releases).

Examples:
  - [QtCaptureTest](./QtCaptureTest/) is a cross platform test program for OpenPnP implemented with Qt.
  - openpnp-capture includes simple examples for [Mac](./mac/tests/), [Linux](./linux/tests/), and [Windows](./win/tests/).

See below for information about Building OpenPnP Capture. 

Documentation for openpnp-capture needs improvement. We would love your [help!](https://github.com/openpnp/openpnp-capture/edit/master/README.md)


# Raw MJPEG Capture

In addition to the standard RGB capture API, openpnp-capture supports **raw MJPEG frame access** — retrieving compressed JPEG bytes directly from the camera without decoding. This is useful for high-performance multi-camera recording and streaming where:

- You want to minimize per-frame CPU work (no JPEG→RGB decode in the hot path)
- You want smaller payload sizes for storage and network (compressed JPEG vs uncompressed RGB24)
- You want to defer decode to when pixels are actually needed (e.g., preview at 15fps while recording at 60fps)

## API

```c
// Open a stream in raw MJPEG mode (returns -1 if format is not MJPEG)
CapStream Cap_openStreamRaw(CapContext ctx, CapDeviceID index, CapFormatID formatID);

// Copy the most recent raw JPEG frame into caller's buffer.
// Sets *outBytes to the actual JPEG byte count (valid even on error).
CapResult Cap_captureFrameRaw(CapContext ctx, CapStream stream,
    void *jpegBufferPtr, uint32_t jpegBufferBytes, uint32_t *outBytes);

// Get the byte size of the current raw frame without copying it
CapResult Cap_getFrameSize(CapContext ctx, CapStream stream, uint32_t *outBytes);

// Decode the current raw JPEG frame into 24-bit RGB on demand
CapResult Cap_decodeFrame(CapContext ctx, CapStream stream,
    void *RGBbufferPtr, uint32_t RGBbufferBytes);
```

## Usage Pattern

```c
// 1. Enumerate formats to find an MJPEG-capable one
CapFormatInfo info;
for (int j = 0; j < Cap_getNumFormats(ctx, deviceID); j++) {
    Cap_getFormatInfo(ctx, deviceID, j, &info);
    if (info.fourcc == 0x47504A4D) { /* MJPG */ mjpegFormat = j; break; }
}

// 2. Open raw stream
CapStream stream = Cap_openStreamRaw(ctx, deviceID, mjpegFormat);

// 3. Pre-allocate a JPEG buffer (JPEG is typically ~25% of uncompressed RGB)
uint32_t estimatedSize = info.width * info.height * 3 / 4;
uint8_t *jpegBuf = malloc(estimatedSize);

// 4. Capture raw frames
while (running) {
    if (Cap_hasNewFrame(ctx, stream)) {
        uint32_t actualBytes = 0;
        CapResult r = Cap_captureFrameRaw(ctx, stream, jpegBuf, estimatedSize, &actualBytes);
        if (r == CAPRESULT_ERR) {
            // Buffer too small — actualBytes tells us the required size
            jpegBuf = realloc(jpegBuf, actualBytes * 2);
            estimatedSize = actualBytes * 2;
            Cap_captureFrameRaw(ctx, stream, jpegBuf, estimatedSize, &actualBytes);
        }
        // jpegBuf now contains a complete JPEG (validated by 0xFF 0xD8 header)
        // Write to disk, stream over network, etc.
    }
    Sleep(1);
}

// 5. Decode to RGB on demand (e.g., for live preview)
uint8_t *rgbBuf = malloc(info.width * info.height * 3);
Cap_decodeFrame(ctx, stream, rgbBuf, info.width * info.height * 3);

// 6. Cleanup
Cap_closeStream(ctx, stream);
Cap_releaseContext(ctx);
```

## Buffer Management

The library uses a **dynamic growth** strategy for its internal JPEG buffer:

- Initial capacity: `width × height × 3 / 4` (conservative JPEG size estimate)
- If a frame exceeds capacity, the buffer grows to `2× the frame size`
- After 1-2 frames, the buffer stabilizes — JPEG sizes don't vary much frame-to-frame
- The caller's buffer works the same way: if your buffer is too small, `Cap_captureFrameRaw` returns `CAPRESULT_ERR` and sets `*outBytes` to the required size

## Platform Details

| Platform | How raw MJPEG is delivered |
|----------|---------------------------|
| **Linux (V4L2)** | MJPEG frames arrive from the kernel via mmap. In raw mode, the library stores them directly — no libjpeg-turbo decode. |
| **Windows (DirectShow)** | Sample Grabber is configured for `MEDIASUBTYPE_MJPG` instead of `MEDIASUBTYPE_RGB24`. DirectShow passes raw compressed bytes without inserting a decoder. |
| **macOS (AVFoundation)** | `AVCaptureVideoDataOutput` is configured with `videoSettings = nil`, requesting native format. Compressed frames arrive in a `CMBlockBuffer`. |

The library automatically configures the correct OS-level pipeline based on `Cap_openStreamRaw` vs `Cap_openStream`.

## Limitations

- **MJPEG formats only.** Raw mode requires an MJPEG-capable format (fourcc `MJPG`). Non-MJPEG formats return -1 from `Cap_openStreamRaw`.
- **Single-frame buffer.** The library stores the latest frame only. If your processing is slower than the camera's framerate, frames will be overwritten.
- **Decode is optional.** `Cap_captureFrame` (RGB) returns `CAPRESULT_ERR` on raw streams. Use `Cap_decodeFrame` for on-demand JPEG→RGB conversion.

---

# Building OpenPnP Capture
## Dependencies
* CMAKE 3.1 or better
* MAKE (osx, linux)
* Visual Studio 2013 + NMake or Ninja Build (windows)
* NASM for building libjpeg-turbo (linux)
* libgtk-3-dev (linux, test program)

## Build instructions (Windows)
Run the batch file 'bootstrap.bat' and choose the desired build system (VisualStudio/nmake or Ninja). Make sure the compiler (Visual Studio) is in the search path. 

Go to the build directory and run nmake or ninja to build the library and the test application.

## Build instructions (OSX)
Run 'bootstrap_osx.sh'. Run make.

## Build instructions (Linux)
Run 'bootstrap_linux.sh'. Run make.


# Supporting Other Platforms
* Implement all PlatformXXX classes, like in the win or linux directories.
* PlatformContext handles device and internal frame buffer format enumeration.
* PlatformStream is responsible for capturing and decoding the camera stream to a 8-bit per channel RGB frame buffer.
* Statically link external dependencies.


# Releases
Releases are built automatically for new tags on all supported platforms using [Github Actions](https://github.com/openpnp/openpnp-capture/blob/master/.github/workflows/build.yml). See https://github.com/openpnp/openpnp-capture/releases/latest to download the latest.

# Platform Notes

## MacOS

On MacOS as of 10.15 Camera permission is needed to open the camera. The library will automatically
execute the permission request, but an Info.plist is required to exist in the application bundle.
An example Info.plist is:

```
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>NSCameraUsageDescription</key>
	<string>openpnp-capture needs permission to access the camera to capture images.</string>
</dict>
</plist>
```

You can reset the camera permissions in MacOS for testing purposes by running ` tccutil reset Camera`.
