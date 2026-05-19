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
| Re-Enumeration           |    Yes     |     Yes      |      Yes       |
| Availability probe       |    Yes     |     Yes      |      Yes       |


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

# Device Availability & Re-enumeration

Three new API functions provide camera availability probing and runtime device list refresh.

## API

```c
// Check whether a camera device is likely available for use.
// Returns CAPRESULT_OK if available, CAPRESULT_ERR if in use or unavailable,
// CAPRESULT_DEVICENOTFOUND if the index is out of range.
DLLPUBLIC CapResult Cap_isDeviceAvailable(CapContext ctx, CapDeviceID index);

// Refresh the device list to reflect currently attached/removed cameras.
// After this call, re-query Cap_getDeviceCount / Cap_getDeviceName —
// device indices may change. Open streams are NOT affected.
DLLPUBLIC CapResult Cap_refreshDevices(CapContext ctx);

// Check whether the device backing an open stream is still physically
// connected. Returns CAPRESULT_OK if present, CAPRESULT_ERR if disconnected
// or the stream is invalid.
DLLPUBLIC CapResult Cap_isDeviceStillConnected(CapContext ctx, CapStream stream);
```

## Usage Pattern

```c
CapContext ctx = Cap_createContext();

// ── Check availability before opening ──────────────────────────────
if (Cap_isDeviceAvailable(ctx, 0) == CAPRESULT_OK) {
    CapStream stream = Cap_openStream(ctx, 0, 0);
    // ... capture ...
}

// ── Poll for disconnection ──────────────────────────────────────────
if (Cap_isDeviceStillConnected(ctx, stream) != CAPRESULT_OK) {
    printf("Camera disconnected!\n");
}

// ── Detect hotplug ──────────────────────────────────────────────────
Cap_refreshDevices(ctx);
uint32_t newCount = Cap_getDeviceCount(ctx);
// re-query device names/IDs — indices may have changed
```

## Platform Semantics

| Platform | "Available" checks | "Still connected" checks |
|---|---|---|
| **Linux** | Opens `/dev/videoN` with `O_RDWR \| O_NONBLOCK`; returns unavailable if `EBUSY` | `VIDIOC_QUERYCAP` ioctl on the open fd |
| **macOS** | `-[AVCaptureDevice isConnected]` + `isInUseByAnotherApplication` | `-[AVCaptureDevice isConnected]` |
| **Windows** | Re-enumerates DirectShow devices, binds filter, verifies capture/preview pin | Re-enumerates DirectShow to check device path still exists |

## Thread Safety

`Cap_refreshDevices` uses internal locking. However, it **must not** be called
concurrently with `Cap_getDeviceCount`, `Cap_getDeviceName`, `Cap_getDeviceUniqueID`,
`Cap_getNumFormats`, `Cap_getFormatInfo`, or `Cap_openStream` from another thread.

## Test Programs

Two test executables exercise the new functionality:

### `availability_test.exe` — Disconnect & reconnect

```bash
# Automated checks (null safety, index validation, refresh stability)
availability_test.exe

# Interactive: prompts you to physically unplug/replug a camera
availability_test.exe --interactive --camera 2
```

### `inuse_test.exe` — Quiet probe vs definitive check

Demonstrates the TWO TIERS of availability detection:

- **Tier 1** — `Cap_isDeviceAvailable()`: fast (~ms), non-invasive, no sensor power-on
- **Tier 2** — `Cap_openStream()` + capture frame + close: slow (~500ms+), powers on sensor, proves real access

```bash
# Compare Tier 1 vs Tier 2 for ALL cameras (parallel, ~3s total)
inuse_test.exe

# Test only camera 2
inuse_test.exe --camera 2

# Interactive OBS/Windows Camera in-use test
inuse_test.exe --interactive
```

### Two-tier pattern for consumers

For best results, use the two-tier pattern:

```c
// Tier 1 — fast scan of all cameras
for (int i = 0; i < Cap_getDeviceCount(ctx); i++) {
    if (Cap_isDeviceAvailable(ctx, i) == CAPRESULT_OK) {
        // Camera likely free — candidate for opening
    }
}

// Tier 2 — definitive check before committing
CapStream s = Cap_openStream(ctx, chosenDevice, chosenFormat);
if (s >= 0) {
    // Camera is truly available — start capturing
} else {
    // Camera is locked by another app (or broken) — try another
}
```

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

# Pre-built static libraries

This fork produces static library archives for linking into Rust (via `build.rs`)
or other languages that consume C static libraries. Archives are built automatically
when a `build.*` tag is pushed and attached to the corresponding
[GitHub Release](https://github.com/jonmatthis/openpnp-capture/releases).

### Repo setup

The release workflow needs write access to create releases. GitHub now defaults
new repos to read-only. To enable it:

1. Go to **Settings → Actions → General**
2. Under **Workflow Permissions**, select **"Read and write permissions"**
3. Save

Without this, the `permissions: contents: write` declared in the workflow file
cannot take effect, and the release upload step will fail with a 403.

## Download URL pattern

```
https://github.com/jonmatthis/openpnp-capture/releases/download/build.N/openpnp-capture-<target>.<ext>
```

where `build.N` is a build number tag (e.g. `build.0`, `build.1`, etc.).

| Target | Extension |
|--------|-----------|
| `windows-x86_64` | `.zip` |
| `windows-arm64` | `.zip` |
| `macos-x86_64` | `.tar.gz` |
| `macos-arm64` | `.tar.gz` |
| `linux-x86_64` | `.tar.gz` |
| `linux-arm64` | `.tar.gz` |

## Archive contents

Every archive contains the same internal structure:

```
lib/
  openpnp-capture.lib    (.a on macOS/Linux)
  turbojpeg-static.lib   (.a on macOS/Linux)
include/
  openpnp-capture.h
```

## Consuming from a Rust build.rs

Add these build dependencies to `Cargo.toml`:

```toml
[build-dependencies]
ureq = "2"                # or your preferred HTTP client
```

For zip/tar.gz extraction, use `std::process::Command` to shell out to
`tar` (Unix) or `powershell Expand-Archive` (Windows) — no extra crate needed.

In `build.rs`:

```rust
use std::env;
use std::path::PathBuf;
use std::process::Command;

const OPENPNP_BUILD: &str = "build.0"; // bump when C code changes

fn main() {
    let target = env::var("TARGET").unwrap();
    let (platform, ext) = if target.contains("windows") {
        let arch = if target.contains("aarch64") { "arm64" } else { "x86_64" };
        (format!("windows-{}", arch), "zip")
    } else if target.contains("apple") {
        let arch = if target.contains("aarch64") { "arm64" } else { "x86_64" };
        (format!("macos-{}", arch), "tar.gz")
    } else if target.contains("linux") {
        let arch = if target.contains("aarch64") { "arm64" } else { "x86_64" };
        (format!("linux-{}", arch), "tar.gz")
    } else {
        panic!("unsupported target: {}", target);
    };

    let url = format!(
        "https://github.com/jonmatthis/openpnp-capture/releases/download/{}/openpnp-capture-{}.{}",
        OPENPNP_BUILD, platform, ext
    );

    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    let extract_dir = out_dir.join("openpnp-capture");

    // Download and extract (skip if already cached)
    if !extract_dir.exists() {
        let archive = out_dir.join(format!("openpnp-capture.{}", ext));

        let body = ureq::get(&url)
            .call()
            .expect("failed to download openpnp-capture")
            .into_body();
        let mut file = std::fs::File::create(&archive).unwrap();
        std::io::copy(&mut body.into_reader(), &mut file).unwrap();

        if ext == "zip" {
            Command::new("powershell")
                .args(["Expand-Archive", "-Path", &archive.to_str().unwrap(),
                       "-DestinationPath", &extract_dir.to_str().unwrap()])
                .status().unwrap();
        } else {
            Command::new("tar")
                .args(["-xzf", &archive.to_str().unwrap(), "-C", &extract_dir.to_str().unwrap()])
                .status().unwrap();
        }
    }

    println!("cargo:rustc-link-search=native={}", extract_dir.join("lib").display());
    println!("cargo:rustc-link-lib=static=openpnp-capture");
    println!("cargo:rustc-link-lib=static=turbojpeg-static");

    // Platform-specific system libraries
    if target.contains("windows") {
        println!("cargo:rustc-link-lib=strmiids");
    }
    if target.contains("apple") {
        println!("cargo:rustc-link-lib=framework=AVFoundation");
        println!("cargo:rustc-link-lib=framework=Foundation");
        println!("cargo:rustc-link-lib=framework=CoreMedia");
        println!("cargo:rustc-link-lib=framework=CoreVideo");
        println!("cargo:rustc-link-lib=framework=Accelerate");
        println!("cargo:rustc-link-lib=framework=IOKit");
    }
}
```

## Bumping the build number

When the C source in this repository changes, push a new tag.

### Option 1 — git alias (recommended)

Add a `bump` alias to your local git config:

```bash
git config alias.bump '!git tag build.$(git tag -l "build.*" | sed "s/build\.//" | sort -n | tail -1 | awk "{print \$0+1}") && git push origin build.$(git tag -l "build.*" | sed "s/build\.//" | sort -n | tail -1 | awk "{print \$0+1}")'
```

Then just run `git bump` — it finds the highest `build.N`, creates `build.N+1`, and pushes it. No thought required.

### Option 2 — manual

Check the last build tag, then push the next one:

```
git tag -l "build.*" | sort -V | tail -1   # → build.0
git tag build.1 && git push origin build.1
```

After either method, update the `OPENPNP_BUILD` constant in your downstream
`build.rs` to match the new tag.

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
