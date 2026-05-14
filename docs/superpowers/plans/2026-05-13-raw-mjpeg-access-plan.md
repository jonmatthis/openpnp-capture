# Raw MJPEG Access Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add raw MJPEG frame access to openpnp-capture so callers can retrieve compressed JPEG bytes without decode, plus on-demand decode when RGB pixels are needed.

**Architecture:** Approach A from spec — parallel raw buffer on `Stream` base class. A `m_rawMode` flag set at open time controls whether platform code stores raw MJPEG or decodes to RGB. Four new C API functions. All existing API unchanged.

**Tech Stack:** C++11 for core, C API boundary, DirectShow (Win), V4L2 (Linux), AVFoundation (macOS), libjpeg-turbo (cross-platform decode-on-demand)

**Spec reference:** `docs/superpowers/specs/2026-05-13-raw-mjpeg-access-design.md`

---

### Task 1: Promote MJPEGHelper from linux/ to common/ + CMake updates

**Files:**
- Move: `linux/mjpeghelper.h` → `common/mjpeghelper.h`
- Move: `linux/mjpeghelper.cpp` → `common/mjpeghelper.cpp`
- Modify: `common/mjpeghelper.h` (update include path)
- Modify: `common/mjpeghelper.cpp` (update include path)
- Modify: `CMakeLists.txt` (move mjpeghelper.cpp to common sources; add libjpeg-turbo link for all platforms)
- Modify: `linux/platformstream.cpp` (update include path)

- [ ] **Step 1: Move the files**

Run:
```bash
cp linux/mjpeghelper.h common/mjpeghelper.h
cp linux/mjpeghelper.cpp common/mjpeghelper.cpp
```

- [ ] **Step 2: Update include paths in the moved files**

In `common/mjpeghelper.h`, change include guard and any references:
```cpp
// old: #include "../common/logging.h" (in mjpeghelper.cpp)
// change to:
#include "logging.h"
```

In `common/mjpeghelper.cpp`, change line 30-31:
```cpp
#include "mjpeghelper.h"
#include "logging.h"
```

- [ ] **Step 3: Update Linux platform stream include**

In `linux/platformstream.h`, change line 41:
```cpp
// old:
#include "mjpeghelper.h"
// new:
#include "../common/mjpeghelper.h"
```

- [ ] **Step 4: Update CMakeLists.txt — move mjpeghelper.cpp to common sources**

In `CMakeLists.txt` line 85-88, change the `add_library` block:
```cmake
add_library(openpnp-capture ${LIBRARY_TYPE} common/libmain.cpp
                                           common/context.cpp
                                           common/logging.cpp
                                           common/stream.cpp
                                           common/mjpeghelper.cpp)
```

And in line 154, remove `linux/mjpeghelper.cpp` from the Linux sources:
```cmake
target_sources(openpnp-capture PRIVATE linux/platformcontext.cpp
                                       linux/platformstream.cpp
                                       linux/yuvconverters.cpp)
```

- [ ] **Step 5: Move libjpeg-turbo setup to platform-independent section in CMakeLists.txt**

The existing libjpeg-turbo find + ExternalProject logic (lines ~158-210) is inside the Linux `ELSEIF(UNIX)` block. It needs to be moved BEFORE the platform-specific sections (before line 99 `IF (WIN32)`) so all three platforms get it. Also remove the Linux-specific `-fPIC` and `-O3` flags from the shared logic, making them conditional on UNIX.

**Remove lines 158-210** from inside the Linux `ELSEIF(UNIX)` block (the entire `find_package`/`pkg_search_module`/`ExternalProject_Add` block + `target_link_libraries` for turbojpeg + `add_dependencies`).

**Remove line 157** (`include_directories(SYSTEM ...)` — no longer needed since we handle includes in the shared block).

**Insert the following block BEFORE line 99** (`IF (WIN32)`):

```cmake
# ======== libjpeg-turbo setup for ALL platforms ========
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_search_module(TurboJPEG libturbojpeg)
endif()
if(TurboJPEG_FOUND)
    target_link_directories(openpnp-capture PRIVATE ${TurboJPEG_LIBDIR})
    target_include_directories(openpnp-capture PRIVATE ${TurboJPEG_INCLUDE_DIRS})
    target_link_libraries(openpnp-capture PRIVATE ${TurboJPEG_LIBRARIES})
else()
    include(ExternalProject)
    set(PIC_FLAG "")
    set(RELEASE_FLAGS "")
    if(UNIX)
        set(PIC_FLAG "-DCMAKE_C_FLAGS=-fPIC" "-DCMAKE_CXX_FLAGS=-fPIC")
        set(RELEASE_FLAGS "-DCMAKE_C_FLAGS_RELEASE=-O3" "-DCMAKE_CXX_FLAGS_RELEASE=-O3")
    endif()
    set(LIBJPEG_TURBO_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/libjpeg-turbo)
    set(LIBJPEG_TURBO_INSTALL_DIR ${LIBJPEG_TURBO_PREFIX}/install)
    ExternalProject_Add(libjpeg-turbo-external
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/linux/contrib/libjpeg-turbo-3.1.2
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX=${LIBJPEG_TURBO_INSTALL_DIR}
            -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DENABLE_SHARED=OFF
            -DWITH_SIMD=ON
            -DWITH_TOOLS=OFF
            -DWITH_JAVA=OFF
            ${PIC_FLAG}
            ${RELEASE_FLAGS}
        BUILD_BYPRODUCTS
            ${LIBJPEG_TURBO_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}turbojpeg${CMAKE_STATIC_LIBRARY_SUFFIX}
    )
    add_library(turbojpeg-static STATIC IMPORTED)
    set_target_properties(turbojpeg-static PROPERTIES
        IMPORTED_LOCATION ${LIBJPEG_TURBO_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}turbojpeg${CMAKE_STATIC_LIBRARY_SUFFIX}
    )
    target_include_directories(openpnp-capture PRIVATE ${LIBJPEG_TURBO_INSTALL_DIR}/include)
    add_dependencies(openpnp-capture libjpeg-turbo-external)
    add_dependencies(turbojpeg-static libjpeg-turbo-external)
    target_link_libraries(openpnp-capture PRIVATE turbojpeg-static)
endif()
# ======================================================
```

The simplified Linux block (after the move) becomes just:
```cmake
ELSEIF(UNIX)
    include(GNUInstallDirs)
    add_definitions(-D__PLATFORM__="Linux ${COMPILERBITS}")

    target_sources(openpnp-capture PRIVATE linux/platformcontext.cpp
                                           linux/platformstream.cpp
                                           linux/yuvconverters.cpp)

    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(openpnp-capture PRIVATE Threads::Threads)

    # install targets unchanged ...
ENDIF()
```
```

The Linux block then simplifies to:
```cmake
ELSEIF(UNIX)
    include(GNUInstallDirs)
    add_definitions(-D__PLATFORM__="Linux ${COMPILERBITS}")

    target_sources(openpnp-capture PRIVATE linux/platformcontext.cpp
                                           linux/platformstream.cpp
                                           linux/yuvconverters.cpp)

    # add pthreads library
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(openpnp-capture PRIVATE Threads::Threads)

    # install targets (unchanged)
    ...
ENDIF()
```

- [ ] **Step 6: Remove old linux/mjpeghelper.cpp and linux/mjpeghelper.h**

Now that all code references `common/mjpeghelper.h`, remove the originals:

```bash
rm linux/mjpeghelper.h linux/mjpeghelper.cpp
```

- [ ] **Step 7: Verify include paths are consistent**

Run:
```bash
grep -rn "mjpeghelper" --include="*.cpp" --include="*.h" --include="*.mm"
```

Expected: only references to `common/mjpeghelper.h` or `mjpeghelper.h` (with common/ in the include path). No references to `linux/mjpeghelper.h`.

---

### Task 2: Add raw buffer members and methods to Stream base class

**Files:**
- Modify: `common/stream.h`
- Modify: `common/stream.cpp`

- [ ] **Step 1: Add new protected members to `common/stream.h`**

After line 127 (`uint32_t m_frames;`), add:

```cpp
    bool        m_rawMode;                  // raw mode (store JPEG, skip decode)
    std::vector<uint8_t> m_rawBuffer;       // compressed JPEG frame storage
    uint32_t    m_rawFrameSize;             // actual byte count of current JPEG frame
    MJPEGHelper m_mjpegHelper;              // on-demand JPEG → RGB decoder
```

- [ ] **Step 2: Add public accessor and new method declarations**

After line 61 (`bool hasNewFrame();`), add:

```cpp
    /** Returns true if stream is in raw (JPEG passthrough) mode */
    bool isRawMode() const { return m_rawMode; }

    /** Copy the most recent raw JPEG frame into caller's buffer.
        Sets *actualBytes to the actual JPEG byte count.
        Returns false if buffer is too small (actualBytes still set). */
    bool captureFrameRaw(uint8_t *outPtr, uint32_t outBytes, uint32_t *actualBytes);

    /** Get the byte size of the current raw frame without copying.
        Returns false if no frame has been received yet. */
    bool getFrameSize(uint32_t *outBytes);

    /** Decode the current raw JPEG frame into 24-bit RGB on demand.
        Only valid on raw-mode streams. */
    bool decodeFrame(uint8_t *RGBbufferPtr, uint32_t RGBbufferBytes);
```

- [ ] **Step 3: Add `m_rawMode` initialization in `Stream` constructor**

In `common/stream.cpp`, modify the constructor (line 38-43):

```cpp
Stream::Stream() :
    m_owner(nullptr),
    m_isOpen(false),
    m_frames(0),
    m_newFrame(false),
    m_rawMode(false),
    m_rawFrameSize(0)
{
}
```

- [ ] **Step 4: Add `captureFrame` guard for raw streams**

In `common/stream.cpp`, at the start of `captureFrame` (line 60), add a raw-mode guard:

```cpp
bool Stream::captureFrame(uint8_t *RGBbufferPtr, uint32_t RGBbufferBytes)
{
    if (m_rawMode) {
        LOG(LOG_ERR, "captureFrame called on raw-mode stream. Use captureFrameRaw or decodeFrame.\n");
        return false;
    }
    if (!m_isOpen) return false;
    // ... rest unchanged ...
```

- [ ] **Step 5: Add `submitRawBuffer` method**

In `common/stream.cpp`, after `submitBuffer` (line 106), add:

```cpp
void Stream::submitRawBuffer(const uint8_t *ptr, size_t bytes)
{
    if (ptr == nullptr) return;

    m_bufferMutex.lock();

    // grow buffer with 2x headroom if needed
    if (bytes > m_rawBuffer.size()) {
        size_t newSize = bytes * 2;
        LOG(LOG_WARNING,
            "Raw buffer growing: frame %u bytes exceeds capacity %u, resizing to %u\n",
            static_cast<uint32_t>(bytes),
            static_cast<uint32_t>(m_rawBuffer.size()),
            static_cast<uint32_t>(newSize));
        m_rawBuffer.resize(newSize);
    }

    memcpy(&m_rawBuffer[0], ptr, bytes);
    m_rawFrameSize = static_cast<uint32_t>(bytes);
    m_newFrame = true;
    m_frames++;

    if (m_frames == 1) {
        LOG(LOG_INFO,
            "Raw buffer: first frame received, %u bytes (capacity %u)\n",
            static_cast<uint32_t>(bytes),
            static_cast<uint32_t>(m_rawBuffer.size()));
    }
    LOG(LOG_DEBUG, "Raw frame #%u stored: %u bytes\n", m_frames, static_cast<uint32_t>(bytes));

    m_bufferMutex.unlock();
}
```

- [ ] **Step 6: Add `captureFrameRaw` method**

```cpp
bool Stream::captureFrameRaw(uint8_t *outPtr, uint32_t outBytes, uint32_t *actualBytes)
{
    if (!m_rawMode) {
        LOG(LOG_ERR, "captureFrameRaw called on non-raw stream\n");
        return false;
    }

    m_bufferMutex.lock();
    *actualBytes = m_rawFrameSize;

    if (outBytes < m_rawFrameSize) {
        LOG(LOG_WARNING,
            "captureFrameRaw: caller buffer too small (%u < %u needed)\n",
            outBytes, m_rawFrameSize);
        m_bufferMutex.unlock();
        return false;
    }

    memcpy(outPtr, &m_rawBuffer[0], m_rawFrameSize);
    m_newFrame = false;
    LOG(LOG_DEBUG, "captureFrameRaw: delivering %u bytes to caller\n", m_rawFrameSize);
    m_bufferMutex.unlock();
    return true;
}
```

- [ ] **Step 7: Add `getFrameSize` method**

```cpp
bool Stream::getFrameSize(uint32_t *outBytes)
{
    if (!m_rawMode) return false;

    m_bufferMutex.lock();
    *outBytes = m_rawFrameSize;
    bool ok = (m_rawFrameSize > 0);
    m_bufferMutex.unlock();
    return ok;
}
```

- [ ] **Step 8: Add `decodeFrame` method**

```cpp
bool Stream::decodeFrame(uint8_t *RGBbufferPtr, uint32_t RGBbufferBytes)
{
    if (!m_rawMode) {
        LOG(LOG_ERR, "decodeFrame called on non-raw stream\n");
        return false;
    }

    uint32_t needed = m_width * m_height * 3;
    if (RGBbufferBytes < needed) {
        LOG(LOG_WARNING,
            "decodeFrame: RGB buffer too small (%u < %u needed)\n",
            RGBbufferBytes, needed);
        return false;
    }

    m_bufferMutex.lock();
    if (m_rawFrameSize == 0) {
        LOG(LOG_ERR, "decodeFrame: no raw frame available yet\n");
        m_bufferMutex.unlock();
        return false;
    }

    LOG(LOG_INFO,
        "decodeFrame: decompressing %u JPEG bytes -> %ux%u RGB\n",
        m_rawFrameSize, m_width, m_height);

    bool ok = m_mjpegHelper.decompressFrame(
        &m_rawBuffer[0], m_rawFrameSize,
        RGBbufferPtr, m_width, m_height);

    if (!ok) {
        LOG(LOG_ERR, "decodeFrame: libjpeg-turbo failed on %u byte frame\n", m_rawFrameSize);
    }

    m_bufferMutex.unlock();
    return ok;
}
```

- [ ] **Step 9: Add include for MJPEGHelper in stream.h**

At the top of `common/stream.h`, after line 35 (`#include "logging.h"`), add:
```cpp
#include "mjpeghelper.h"
```

---

### Task 3: Add raw stream dispatch methods to Context

**Files:**
- Modify: `common/context.h`
- Modify: `common/context.cpp`

- [ ] **Step 1: Add declarations to `common/context.h`**

After line 101 (`int32_t openStream(CapDeviceID id, CapFormatID formatID);`), add:

```cpp
    /** Open a stream in raw (JPEG passthrough) mode.
        Only succeeds for MJPEG formats. Returns -1 if format is not MJPEG. */
    int32_t openStreamRaw(CapDeviceID id, CapFormatID formatID);

    /** Copy latest raw JPEG frame. Returns false if buffer too small. */
    bool captureFrameRaw(int32_t streamID, uint8_t *jpegBufferPtr, uint32_t jpegBufferBytes, uint32_t *outBytes);

    /** Get byte size of latest raw frame. */
    bool getFrameSize(int32_t streamID, uint32_t *outBytes);

    /** Decode latest raw JPEG into RGB. */
    bool decodeFrame(int32_t streamID, uint8_t *RGBbufferPtr, uint32_t RGBbufferBytes);
```

- [ ] **Step 2: Implement `openStreamRaw` in `common/context.cpp`**

Add after `openStream` (line 174):

```cpp
int32_t Context::openStreamRaw(CapDeviceID id, CapFormatID formatID)
{
    deviceInfo *device = nullptr;

    if (m_devices.size() > id) {
        device = m_devices[id];
    } else {
        LOG(LOG_ERR, "openStreamRaw: No devices found\n");
        return -1;
    }

    // lookup desired format
    if (formatID >= device->m_formats.size()) {
        LOG(LOG_ERR, "openStreamRaw: Requested format index out of range\n");
        return -1;
    }

    // verify the format is MJPEG
    uint32_t fourcc = device->m_formats[formatID].fourcc;
    if (fourcc != 0x47504A4D) { // MJPG
        LOG(LOG_ERR, "openStreamRaw: Format %s is not MJPEG. Raw mode requires MJPG.\n",
            fourCCToString(fourcc).c_str());
        return -1;
    }

    Stream *s = createPlatformStream();

    // set raw mode BEFORE calling open() so platform code can configure accordingly
    s->m_rawMode = true;

    // pre-size the raw buffer: estimate JPEG at ~25% of uncompressed RGB size
    uint32_t estimatedJPEGSize = device->m_formats[formatID].width *
                                 device->m_formats[formatID].height * 3 / 4;
    s->m_rawBuffer.resize(estimatedJPEGSize);
    LOG(LOG_INFO,
        "Opening stream in RAW mode (device %s, format=%d, fourcc=%s, "
        "%ux%u @ %u fps), initial raw buffer capacity: %u bytes\n",
        device->m_name.c_str(), formatID, fourCCToString(fourcc).c_str(),
        device->m_formats[formatID].width, device->m_formats[formatID].height,
        device->m_formats[formatID].fps, estimatedJPEGSize);

    if (!s->open(this, device, device->m_formats[formatID].width,
                 device->m_formats[formatID].height,
                 device->m_formats[formatID].fourcc,
                 device->m_formats[formatID].fps)) {
        LOG(LOG_ERR, "Could not open raw stream for device %s\n", device->m_name.c_str());
        delete s;
        return -1;
    }

    LOG(LOG_INFO, "Raw stream opened successfully (stream=%d)\n", m_streamCounter);

    int32_t streamID = storeStream(s);
    return streamID;
}
```

Important: We need to make `m_rawMode` accessible to `Context`. Since it's a `protected` member on `Stream`, and `Context` is not a subclass, we need to either:
- Make `Context` a friend of `Stream`, or
- Make `m_rawMode` public, or
- Add a setter `Stream::setRawMode(bool)`

The cleanest is a setter — add to `Stream`:

```cpp
void setRawMode(bool mode) { m_rawMode = mode; }
```

And make `m_rawBuffer` accessible for pre-sizing (or add a setter for that too). Actually, the simplest approach: add public methods on `Stream` that `Context` can call:

In `common/stream.h`, after `isRawMode()`:
```cpp
    void setRawMode(bool mode, uint32_t estimatedJPEGSize) {
        m_rawMode = mode;
        if (mode) {
            m_rawBuffer.resize(estimatedJPEGSize);
        }
    }
```

- [ ] **Step 3: Implement dispatch methods in `common/context.cpp`**

Add after the existing `captureFrame` method (line 226):

```cpp
bool Context::captureFrameRaw(int32_t streamID, uint8_t *jpegBufferPtr, uint32_t jpegBufferBytes, uint32_t *outBytes)
{
    if (streamID < 0) {
        LOG(LOG_ERR, "captureFrameRaw: negative stream ID\n");
        return false;
    }
    Stream *stream = m_streams[streamID];
    if (stream == nullptr) {
        LOG(LOG_ERR, "captureFrameRaw: unknown stream ID\n");
        return false;
    }
    return stream->captureFrameRaw(jpegBufferPtr, jpegBufferBytes, outBytes);
}

bool Context::getFrameSize(int32_t streamID, uint32_t *outBytes)
{
    if (streamID < 0) {
        return false;
    }
    Stream *stream = m_streams[streamID];
    if (stream == nullptr) {
        return false;
    }
    return stream->getFrameSize(outBytes);
}

bool Context::decodeFrame(int32_t streamID, uint8_t *RGBbufferPtr, uint32_t RGBbufferBytes)
{
    if (streamID < 0) {
        LOG(LOG_ERR, "decodeFrame: negative stream ID\n");
        return false;
    }
    Stream *stream = m_streams[streamID];
    if (stream == nullptr) {
        LOG(LOG_ERR, "decodeFrame: unknown stream ID\n");
        return false;
    }
    return stream->decodeFrame(RGBbufferPtr, RGBbufferBytes);
}
```

---

### Task 4: Add new C API functions

**Files:**
- Modify: `include/openpnp-capture.h`
- Modify: `common/libmain.cpp`

- [ ] **Step 1: Add declarations to `include/openpnp-capture.h`**

After line 228 (`DLLPUBLIC uint32_t Cap_getStreamFrameCount(...)`), add:

```c
/** Open a capture stream in raw MJPEG mode.
    Frames are stored as compressed JPEG bytes, not decoded.
    Use Cap_captureFrameRaw to retrieve raw JPEG data.
    Use Cap_decodeFrame to decode on demand into RGB.

    @param ctx The ID of the context.
    @param index The device index of the capture device.
    @param formatID The index/ID of the MJPEG frame buffer format.
    @return The stream ID or -1 if the format is not MJPEG.
*/
DLLPUBLIC CapStream Cap_openStreamRaw(CapContext ctx, CapDeviceID index, CapFormatID formatID);

/** Copy the most recent raw JPEG frame into the caller's buffer.

    @param ctx The ID of the context.
    @param stream The stream ID (must be opened with Cap_openStreamRaw).
    @param jpegBufferPtr Pointer to caller's buffer for the raw JPEG data.
    @param jpegBufferBytes Size of caller's buffer in bytes.
    @param outBytes Receives the actual JPEG byte count (valid even on error).
    @return CAPRESULT_OK on success, CAPRESULT_ERR if buffer too small or not a raw stream.
*/
DLLPUBLIC CapResult Cap_captureFrameRaw(CapContext ctx, CapStream stream,
    void *jpegBufferPtr, uint32_t jpegBufferBytes, uint32_t *outBytes);

/** Get the byte size of the current raw frame without copying it.

    @param ctx The ID of the context.
    @param stream The stream ID (must be opened with Cap_openStreamRaw).
    @param outBytes Receives the size in bytes, or 0 if no frame received yet.
    @return CAPRESULT_OK on success, CAPRESULT_ERR if no frame available or invalid stream.
*/
DLLPUBLIC CapResult Cap_getFrameSize(CapContext ctx, CapStream stream, uint32_t *outBytes);

/** Decode the current raw JPEG frame into 24-bit RGB on demand.

    @param ctx The ID of the context.
    @param stream The stream ID (must be opened with Cap_openStreamRaw).
    @param RGBbufferPtr Pointer to caller's buffer (must hold width*height*3 bytes).
    @param RGBbufferBytes Size of caller's RGB buffer in bytes.
    @return CAPRESULT_OK on success, CAPRESULT_ERR on failure.
*/
DLLPUBLIC CapResult Cap_decodeFrame(CapContext ctx, CapStream stream,
    void *RGBbufferPtr, uint32_t RGBbufferBytes);
```

- [ ] **Step 2: Add implementations to `common/libmain.cpp`**

After line 172 (`Cap_getStreamFrameCount`):

```cpp
DLLPUBLIC CapStream Cap_openStreamRaw(CapContext ctx, CapDeviceID index, CapFormatID formatID)
{
    if (ctx != 0)
    {
        Context *c = reinterpret_cast<Context*>(ctx);
        return c->openStreamRaw(index, formatID);
    }
    return -1;
}

DLLPUBLIC CapResult Cap_captureFrameRaw(CapContext ctx, CapStream stream,
    void *jpegBufferPtr, uint32_t jpegBufferBytes, uint32_t *outBytes)
{
    if (ctx != 0 && outBytes != nullptr)
    {
        Context *c = reinterpret_cast<Context*>(ctx);
        return c->captureFrameRaw(stream, (uint8_t*)jpegBufferPtr, jpegBufferBytes, outBytes)
            ? CAPRESULT_OK : CAPRESULT_ERR;
    }
    return CAPRESULT_ERR;
}

DLLPUBLIC CapResult Cap_getFrameSize(CapContext ctx, CapStream stream, uint32_t *outBytes)
{
    if (ctx != 0 && outBytes != nullptr)
    {
        Context *c = reinterpret_cast<Context*>(ctx);
        return c->getFrameSize(stream, outBytes) ? CAPRESULT_OK : CAPRESULT_ERR;
    }
    return CAPRESULT_ERR;
}

DLLPUBLIC CapResult Cap_decodeFrame(CapContext ctx, CapStream stream,
    void *RGBbufferPtr, uint32_t RGBbufferBytes)
{
    if (ctx != 0)
    {
        Context *c = reinterpret_cast<Context*>(ctx);
        return c->decodeFrame(stream, (uint8_t*)RGBbufferPtr, RGBbufferBytes)
            ? CAPRESULT_OK : CAPRESULT_ERR;
    }
    return CAPRESULT_ERR;
}
```

---

### Task 5: Update Linux platform stream

**Files:**
- Modify: `linux/platformstream.cpp`

- [ ] **Step 1: Add `m_rawMode` branch in the MJPG case of `threadSubmitBuffer`**

In `linux/platformstream.cpp`, at line 568, change the MJPG case:

```cpp
        case 0x47504A4D:    // MJPG
            if (m_rawMode) {
                LOG(LOG_DEBUG, "V4L2: raw mode active — skipping MJPEG decode in threadSubmitBuffer\n"
                    "(frame %u, %u bytes)\n", m_frames, static_cast<uint32_t>(bytes));
                submitRawBuffer((uint8_t*)ptr, bytes);
            } else {
#ifdef FRAMEDUMP
            {
                static int32_t fcnt = 0;
                char fname[100];
                if (fcnt < 10)
                {
                    sprintf(fname,"frame_%d.dat", fcnt++);
                    FILE *fout = fopen(fname, "wb");
                    fwrite(ptr, 1, bytes, fout);
                    fclose(fout);
                }
            }
#endif
                // existing decode path unchanged
                m_bufferMutex.lock();
                if (m_mjpegHelper.decompressFrame((uint8_t*)ptr, bytes, &m_frameBuffer[0], m_width, m_height))
                {
                    m_newFrame = true;
                    m_frames++;
                }
                m_bufferMutex.unlock();
            }
            break;
```

Note: The `#ifdef FRAMEDUMP` block is debug-only and stays inside the `else` branch (non-raw path) since raw mode callers can dump the JPEG themselves.

---

### Task 6: Update Windows platform stream

**Files:**
- Modify: `win/platformstream.cpp`
- Modify: `win/platformstream.h`

- [ ] **Step 1: Set Sample Grabber media type based on raw mode**

In `win/platformstream.cpp`, locate the media type setup (~line 391-396). The exact location is inside the `open()` method where the Sample Grabber media type is configured. Change:

```cpp
    mt.subtype = MEDIASUBTYPE_RGB24;
```

to:

```cpp
    if (m_rawMode) {
        mt.subtype = MEDIASUBTYPE_MJPG;
        LOG(LOG_INFO, "DirectShow: SampleGrabber media type set to MEDIASUBTYPE_MJPG (raw mode)\n");
    } else {
        mt.subtype = MEDIASUBTYPE_RGB24;
    }
```

- [ ] **Step 2: Route `SampleCB` callback to appropriate submit**

In `win/platformstream.cpp`, in `StreamCallbackHandler::SampleCB` (~line 83), change:

```cpp
    m_stream->submitBuffer(ptr, bytes);
```

to:

```cpp
    if (m_stream->isRawMode()) {
        m_stream->submitRawBuffer(ptr, bytes);
    } else {
        m_stream->submitBuffer(ptr, bytes);
    }
```

- [ ] **Step 3: Verify the MJPG format fix code is unaffected**

The MJPG fix commit (`fde0ff1`) adds code that re-applies the MJPG format on the capture pin during `open()`. This code runs *before* the Sample Grabber configuration and is orthogonal — it ensures the camera delivers MJPG at the right resolution/framerate. No changes needed to that code. Verify by reading the area around lines 556-654 to confirm it only touches the capture pin format, not the Sample Grabber media type.

---

### Task 7: Update macOS platform stream

**Files:**
- Modify: `mac/platformstream.mm`
- Modify: `mac/platformstream.h`

- [ ] **Step 1: Set video output settings based on raw mode**

In `mac/platformstream.mm`, inside the `open()` method where `AVCaptureVideoDataOutput` is configured (~line 229-235). Currently it's:

```objc
    output.videoSettings = nil;

    output.videoSettings = [NSDictionary dictionaryWithObjectsAndKeys:
        [NSNumber numberWithUnsignedInt:kCVPixelFormatType_32ARGB], (id)kCVPixelBufferPixelFormatTypeKey,
        nil];
```

The first `output.videoSettings = nil;` line was already there (possibly for another reason). Change this whole section to:

```objc
    if (m_rawMode) {
        output.videoSettings = nil;  // native format delivery (MJPEG for MJPEG cameras)
        LOG(LOG_INFO, "AVFoundation: videoSettings=nil (native format delivery for raw mode)\n");
    } else {
        output.videoSettings = [NSDictionary dictionaryWithObjectsAndKeys:
            [NSNumber numberWithUnsignedInt:kCVPixelFormatType_32ARGB], (id)kCVPixelBufferPixelFormatTypeKey,
            nil];
    }
```

- [ ] **Step 2: Route delegate callback based on raw mode**

In `mac/platformstream.mm`, in the `captureOutput:didOutputSampleBuffer:fromConnection:` delegate method (~line 48). The existing code gets a `CVPixelBufferRef` for the RGB path. For raw mode, we need to extract from the `CMBlockBuffer` instead.

The current callback structure (~lines 48-97):

```objc
- (void)captureOutput:(AVCaptureOutput *)captureOutput
        didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
        fromConnection:(AVCaptureConnection *)connection
{
    CMItemCount count = CMSampleBufferGetNumSamples(sampleBuffer);
    if (count < 1) return;

    if (m_stream != nullptr)
    {
        CMFormatDescriptionRef desc = CMSampleBufferGetFormatDescription(sampleBuffer);
        FourCharCode fourcc = CMFormatDescriptionGetMediaSubType(desc);
        CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(desc);

        // ... existing validation ...

        CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
        if (CVPixelBufferLockBaseAddress(pixelBuffer, 0) == kCVReturnSuccess)
        {
            const uint8_t *pixelPtr = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixelBuffer));
            // ... existing callback call ...
            CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
        }
    }
}
```

Add a raw-mode branch at the top of the `if (m_stream != nullptr)` block:

```objc
    if (m_stream != nullptr)
    {
        if (m_stream->isRawMode())
        {
            // Extract raw compressed data from CMBlockBuffer
            CMBlockBufferRef blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
            if (blockBuffer != NULL) {
                size_t length = 0;
                char *dataPtr = NULL;
                OSStatus status = CMBlockBufferGetDataPointer(blockBuffer, 0,
                    NULL, &length, &dataPtr);
                if (status == kCMBlockBufferNoErr && dataPtr != NULL) {
                    m_stream->submitRawBuffer((uint8_t*)dataPtr, length);
                } else {
                    LOG(LOG_ERR, "AVFoundation: CMBlockBufferGetDataPointer failed: %d\n", (int)status);
                }
            } else {
                LOG(LOG_ERR, "AVFoundation: CMSampleBufferGetDataBuffer returned NULL in raw mode\n");
            }
            return;
        }

        // ... existing ARGB path unchanged ...
    }
```

---

### Task 8: Build verification

Since we can't test with real camera hardware on all platforms, verify the code at least compiles cleanly.

- [ ] **Step 1: Verify the file structure is correct**

Run:
```bash
ls common/mjpeghelper.h common/mjpeghelper.cpp
```

- [ ] **Step 2: Scan for missing includes or typos**

Run:
```bash
# Check all modified files reference correct includes
grep -rn "^#include" common/stream.h common/stream.cpp common/context.h common/context.cpp \
    include/openpnp-capture.h common/libmain.cpp common/mjpeghelper.h common/mjpeghelper.cpp \
    linux/platformstream.h linux/platformstream.cpp \
    win/platformstream.h win/platformstream.cpp \
    mac/platformstream.h mac/platformstream.mm
```

- [ ] **Step 3: Review the complete change set**

A quick scan of all changes to verify interoperability — these are the contracts between tasks that must be consistent:

| Contract | Provided by | Consumed by |
|----------|------------|-------------|
| `Stream::m_rawMode` + `isRawMode()` | Task 2 (stream.h) | Tasks 5, 6, 7 (all platforms), Task 3 (Context) |
| `Stream::setRawMode(bool, uint32_t)` | Task 2 (stream.h) | Task 3 (Context::openStreamRaw) |
| `Stream::submitRawBuffer(ptr, bytes)` | Task 2 (stream.cpp) | Tasks 5, 6, 7 (all platforms) |
| `Stream::captureFrameRaw(outPtr, outBytes, actualBytes)` | Task 2 (stream.cpp) | Task 3 (Context), Task 4 (libmain.cpp) |
| `Stream::getFrameSize(outBytes)` | Task 2 (stream.cpp) | Task 3 (Context), Task 4 (libmain.cpp) |
| `Stream::decodeFrame(RGBptr, RGBbytes)` | Task 2 (stream.cpp) | Task 3 (Context), Task 4 (libmain.cpp) |
| `Stream::captureFrame` guard for raw mode | Task 2 (stream.cpp) | Existing callers |
| `Context::openStreamRaw` | Task 3 (context.cpp) | Task 4 (libmain.cpp) |
| `MJPEGHelper` in `common/` | Task 1 | Task 2 (stream.h includes it) |
| libjpeg-turbo linked on all platforms | Task 1 (CMakeLists.txt) | Task 2 (decodeFrame uses turbojpeg) |

---

## Implementation Order

Tasks must be completed in this order due to dependencies:

```
Task 1 (MJPEGHelper + CMake) → Task 2 (Stream base class) → Task 3 (Context dispatch)
                                                                ↓
                                                          Task 4 (C API)
                                                                ↓
                                              Task 5 (Linux), Task 6 (Win), Task 7 (Mac)
                                              (all three can be done in parallel)
                                                                ↓
                                                          Task 8 (Verification)
```

Tasks 5, 6, 7 are independent of each other and can be done in parallel.
