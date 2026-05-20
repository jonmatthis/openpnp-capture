/*

    OpenPnp-Capture: a video capture subsystem.

    Copyright (c) 2017 Jason von Nieda, Niels Moseley.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

/*!
*  @file
*  @brief C API for OpenPnP Capture Library
*/

#ifndef openpnp_capture_h
#define openpnp_capture_h

#include <stdint.h>

// 
//
#if defined(__clang__)
    #define SO_IMPORT 
    #define SO_EXPORT    
#elif defined(__GNUC__) || defined(__GNUG__)
    #define SO_IMPORT
    #define SO_EXPORT
#elif defined(_MSC_VER)
    #ifndef OPENPNPCAPTURE_STATIC
        #define SO_IMPORT __declspec(dllimport)
        #define SO_EXPORT __declspec(dllexport)
    #else
        #define SO_IMPORT
        #define SO_EXPORT
    #endif
#else
    #error("Unknown compiler")
#endif

// make sure its exported/imported as pure C 
// even if we're compiling with a C++ compiler
#ifdef BUILD_OPENPNP_LIBRARY
    #ifdef __cplusplus
        #define DLLPUBLIC extern "C" SO_EXPORT
    #else
        #define DLLPUBLIC SO_EXPORT
    #endif
#else
    #ifdef __cplusplus
        #define DLLPUBLIC extern "C" SO_IMPORT
    #else
        #define DLLPUBLIC SO_IMPORT
    #endif
#endif

typedef void*    CapContext;    ///< an opaque pointer to the internal Context*
typedef int32_t  CapStream;     ///< a stream identifier (normally >=0, <0 for error)
typedef uint32_t CapResult;     ///< result defined by CAPRESULT_xxx
typedef uint32_t CapDeviceID;   ///< unique device ID
typedef uint32_t CapFormatID;   ///< format identifier 0 .. numFormats

// supported properties:
#define CAPPROPID_EXPOSURE      1
#define CAPPROPID_FOCUS         2 
#define CAPPROPID_ZOOM          3
#define CAPPROPID_WHITEBALANCE  4
#define CAPPROPID_GAIN          5
#define CAPPROPID_BRIGHTNESS    6
#define CAPPROPID_CONTRAST      7
#define CAPPROPID_SATURATION    8
#define CAPPROPID_GAMMA         9
#define CAPPROPID_HUE           10
#define CAPPROPID_SHARPNESS     11
#define CAPPROPID_BACKLIGHTCOMP 12
#define CAPPROPID_POWERLINEFREQ 13
#define CAPPROPID_LAST          14

typedef uint32_t CapPropertyID; ///< property ID (exposure, zoom, focus etc.)

typedef struct
{
    uint32_t width;     ///< width in pixels
    uint32_t height;    ///< height in pixels
    uint32_t fourcc;    ///< fourcc code (platform dependent)
    uint32_t fps;       ///< frames per second (one of potentially several
                        ///  supported rates for this resolution; use
                        ///  Cap_getNumFormats / Cap_getFormatInfo to enumerate
                        ///  all available FPS values for a given resolution)
    uint32_t bpp;       ///< bits per pixel
} CapFormatInfo;

#define CAPRESULT_OK  0
#define CAPRESULT_ERR 1
#define CAPRESULT_DEVICENOTFOUND 2
#define CAPRESULT_FORMATNOTSUPPORTED 3
#define CAPRESULT_PROPERTYNOTSUPPORTED 4

/********************************************************************************** 
     CONTEXT CREATION AND DEVICE ENUMERATION
**********************************************************************************/

/** Initialize the capture library
    @return The context ID.
*/
DLLPUBLIC CapContext Cap_createContext(void);

/** Un-initialize the capture library context
    @param ctx The ID of the context to destroy.
    @return The context ID.
*/
DLLPUBLIC CapResult Cap_releaseContext(CapContext ctx);

/** Get the number of capture devices on the system.
    note: this can change dynamically due to the
    pluggin and unplugging of USB devices.
    @param ctx The ID of the context.
    @return The number of capture devices found.
*/
DLLPUBLIC uint32_t Cap_getDeviceCount(CapContext ctx);

/** Get the name of a capture device.
    This name is meant to be displayed in GUI applications,
    i.e. its human readable.

    if a device with the given index does not exist,
    NULL is returned.
    @param ctx The ID of the context.
    @param index The device index of the capture device.
    @return a pointer to an UTF-8 string containting the name of the capture device.
*/
DLLPUBLIC const char* Cap_getDeviceName(CapContext ctx, CapDeviceID index);

/** Get the unique name of a capture device.
    The string contains a unique concatenation
    of the device name and other parameters.
    These parameters are platform dependent.

    Note: when a USB camera does not expose a serial number,
          platforms might have trouble uniquely identifying 
          a camera. In such cases, the USB port location can
          be used to add a unique feature to the string.
          This, however, has the down side that the ID of
          the camera changes when the USB port location 
          changes. Unfortunately, there isn't much to
          do about this.

    if a device with the given index does not exist,
    NULL is returned.
    @param ctx The ID of the context.
    @param index The device index of the capture device.
    @return a pointer to an UTF-8 string containting the unique ID of the capture device.
*/
DLLPUBLIC const char* Cap_getDeviceUniqueID(CapContext ctx, CapDeviceID index);


/** Returns the number of formats supported by a certain device.
    returns -1 if device does not exist.

    @param ctx The ID of the context.
    @param index The device index of the capture device.
    @return The number of formats supported or -1 if the device does not exist.
*/
DLLPUBLIC int32_t Cap_getNumFormats(CapContext ctx, CapDeviceID index);

/** Get the format information from a device. 
    @param ctx The ID of the context.
    @param index The device index of the capture device.
    @param id The index/ID of the frame buffer format (0 .. number returned by Cap_getNumFormats() minus 1 ).
    @param info pointer to a CapFormatInfo structure to be filled with data.
    @return The CapResult.
*/
DLLPUBLIC CapResult Cap_getFormatInfo(CapContext ctx, CapDeviceID index, CapFormatID id, CapFormatInfo *info);


/**********************************************************************************
     DEVICE AVAILABILITY AND RE-ENUMERATION
**********************************************************************************/

/** Check whether a camera device is likely available for use.

    FAST, NON-INVASIVE PROBE. Does NOT power on the sensor, turn on the
    camera LED, or capture any frames. Runs in ~1ms per device.

    Platform-specific checks:
      - Linux:   opens /dev/videoN, checks for EBUSY, closes immediately.
      - macOS:   reads AVCaptureDevice.isInUseByAnotherApplication.
      - Windows: re-enumerates DirectShow and verifies the device filter
                 can be bound with a capture/preview pin present.

    WHEN TO USE: Batch-scan many cameras quickly to find candidates.
    Use this as a "definitely NOT available" filter — if it returns
    CAPRESULT_ERR, don't bother with the expensive probe.

    WHEN NOT TO USE: Do NOT rely on this alone for a definitive answer.
    CAPRESULT_OK does NOT guarantee the device can deliver frames.
    On Windows especially, DirectShow devices are shareable by default,
    so another app may already be streaming from the same camera.

    For a definitive check, use Cap_probeDevice() (invasive probe) or
    Cap_verifyDevice() (fast probe + invasive, one call).

    @param ctx The ID of the context.
    @param index The device index of the capture device.
    @return CAPRESULT_OK if the device appears available (NOT definitive),
            CAPRESULT_ERR if the device cannot be probed (any reason:
            exclusive lock, missing device, virtual device with no
            DirectShow path, driver error, etc.),
            CAPRESULT_DEVICENOTFOUND if the index is out of range.
*/
DLLPUBLIC CapResult Cap_isDeviceAvailable(CapContext ctx, CapDeviceID index);

/** Perform an invasive probe: open the device, wait for a frame, close it.

    DEFINITIVE AVAILABILITY CHECK. This ACTUALLY OPENS a stream, waits for
    at least one frame to arrive, and closes the stream. This is the only
    reliable way to determine if a camera can deliver frames.

    SIDE EFFECTS: Powers on the sensor, may flash the camera LED, and holds
    the device exclusively for up to timeoutMs. Callers should warn users or
    only call this during initial setup / configuration screens.

    This takes ~500ms+ per camera. For scanning many cameras efficiently,
    first use Cap_isDeviceAvailable() to narrow candidates, then call
    Cap_probeDevice() only on likely-free devices. Or use Cap_verifyDevice()
    to run both checks in a single call.

    @param ctx       The ID of the context.
    @param index     The device index of the capture device.
    @param formatID  The format to test with (0 = first available format).
    @param timeoutMs Maximum time to wait for a frame (0 = default 2000ms).
    @return CAPRESULT_OK if the device was opened and at least one frame
            arrived (the device is truly available),
            CAPRESULT_ERR if the device cannot be opened or produces no
            frames within the timeout,
            CAPRESULT_DEVICENOTFOUND if the index is out of range.
*/
DLLPUBLIC CapResult Cap_probeDevice(CapContext ctx, CapDeviceID index,
    CapFormatID formatID, uint32_t timeoutMs);

/** Convenience: run the fast probe AND the invasive probe in one call.

    Chains Cap_isDeviceAvailable() + Cap_probeDevice(). Returns a definitive
    result — a camera that passes this check can actually be opened and will
    deliver frames.

    This is the function most consumers should use when they want a simple
    "is this camera actually usable?" answer. No need to understand the
    two-tier architecture or call multiple functions.

    SIDE EFFECTS: Same as Cap_probeDevice — powers on the sensor, may flash
    the camera LED. Takes ~500ms+ per device.

    @param ctx       The ID of the context.
    @param index     The device index of the capture device.
    @param formatID  The format to test with (0 = first available format).
    @param timeoutMs Maximum time to wait for a frame (0 = default 2000ms).
    @return CAPRESULT_OK if the device can deliver frames,
            CAPRESULT_ERR if the device fails either the fast or invasive
            check,
            CAPRESULT_DEVICENOTFOUND if the index is out of range.
*/
DLLPUBLIC CapResult Cap_verifyDevice(CapContext ctx, CapDeviceID index,
    CapFormatID formatID, uint32_t timeoutMs);

/** Refresh the device list to reflect currently attached/removed cameras.
    After this call, Cap_getDeviceCount() will reflect the current system state.
    Open streams are NOT affected — their underlying device handles remain valid.

    Callers should re-query device count and names after this call, as device
    indices may change.

    Must not be called concurrently with device-list-accessing functions
    (Cap_getDeviceCount, Cap_getDeviceName, Cap_openStream, etc.).

    @param ctx The ID of the context.
    @return CAPRESULT_OK on success, CAPRESULT_ERR on failure.
*/
DLLPUBLIC CapResult Cap_refreshDevices(CapContext ctx);

/** Check whether the device backing an open stream is still connected.

    On Linux, this runs VIDIOC_QUERYCAP on the open file descriptor.
    On macOS, this checks -[AVCaptureDevice isConnected].
    On Windows, this re-enumerates DirectShow and verifies the device path
    still exists in the system (rather than querying the cached COM filter,
    which survives physical unplug).

    @param ctx The ID of the context.
    @param stream The stream ID to check.
    @return CAPRESULT_OK if the underlying device is still present,
            CAPRESULT_ERR if the device has been disconnected or the stream is invalid.
*/
DLLPUBLIC CapResult Cap_isDeviceStillConnected(CapContext ctx, CapStream stream);


/********************************************************************************** 
     STREAM MANAGEMENT
**********************************************************************************/

/** Open a capture stream to a device with specific format requirements 

    Although the (internal) frame buffer format is set via the fourCC ID,
    the frames returned by Cap_captureFrame are always 24-bit RGB.

    @param ctx The ID of the context.
    @param index The device index of the capture device.
    @param formatID The index/ID of the frame buffer format (0 .. number returned by Cap_getNumFormats() minus 1 ).
    @return The stream ID or -1 if the device does not exist or the stream format ID is incorrect.
*/
DLLPUBLIC CapStream Cap_openStream(CapContext ctx, CapDeviceID index, CapFormatID formatID);

/** Close a capture stream 
    @param ctx The ID of the context.
    @param stream The stream ID.
    @return CapResult
*/
DLLPUBLIC CapResult Cap_closeStream(CapContext ctx, CapStream stream);

/** Check if a stream is open, i.e. is capturing data. 
    @param ctx The ID of the context.
    @param stream The stream ID.
    @return 1 if the stream is open and capturing, else 0. 
*/
DLLPUBLIC uint32_t Cap_isOpenStream(CapContext ctx, CapStream stream);

/********************************************************************************** 
     FRAME CAPTURING / INFO
**********************************************************************************/

/** this function copies the most recent RGB frame data
    to the given buffer.
*/
DLLPUBLIC CapResult Cap_captureFrame(CapContext ctx, CapStream stream, void *RGBbufferPtr, uint32_t RGBbufferBytes);

/** returns 1 if a new frame has been captured, 0 otherwise */
DLLPUBLIC uint32_t Cap_hasNewFrame(CapContext ctx, CapStream stream);

/** returns the number of frames captured during the lifetime of the stream. 
    For debugging purposes */
DLLPUBLIC uint32_t Cap_getStreamFrameCount(CapContext ctx, CapStream stream);

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

/** Get the actual negotiated resolution of an open stream.

    This returns the resolution that DirectShow actually negotiated,
    which may differ from the requested format if the camera driver
    overrides the format during pin connection. ALWAYS call this
    after opening a stream to verify the resolution matches what
    was requested.

    @param ctx The ID of the context.
    @param stream The stream ID.
    @param outWidth Receives the actual width in pixels.
    @param outHeight Receives the actual height in pixels.
    @return CAPRESULT_OK on success, CAPRESULT_ERR if stream invalid or not open.
*/
DLLPUBLIC CapResult Cap_getStreamResolution(CapContext ctx, CapStream stream,
    uint32_t *outWidth, uint32_t *outHeight);


/**********************************************************************************
     NEW CAMERA CONTROL API FUNCTIONS
**********************************************************************************/

/** get the min/max limits and default value of a camera/stream property (e.g. zoom, exposure etc) 

    returns: CAPRESULT_OK if all is well.
             CAPRESULT_PROPERTYNOTSUPPORTED if property not available.
             CAPRESULT_ERR if context, stream are invalid.
*/
DLLPUBLIC CapResult Cap_getPropertyLimits(CapContext ctx, CapStream stream, CapPropertyID propID, 
    int32_t *min, int32_t *max, int *dValue);

/** set the value of a camera/stream property (e.g. zoom, exposure etc) 

    returns: CAPRESULT_OK if all is well.
             CAPRESULT_PROPERTYNOTSUPPORTED if property not available.
             CAPRESULT_ERR if context, stream are invalid.
*/
DLLPUBLIC CapResult Cap_setProperty(CapContext ctx, CapStream stream, CapPropertyID propID, int32_t value);

/** set the automatic flag of a camera/stream property (e.g. zoom, focus etc) 

    returns: CAPRESULT_OK if all is well.
             CAPRESULT_PROPERTYNOTSUPPORTED if property not available.
             CAPRESULT_ERR if context, stream are invalid.
*/
DLLPUBLIC CapResult Cap_setAutoProperty(CapContext ctx, CapStream stream, CapPropertyID propID, uint32_t bOnOff);

/** set the frame rate of an opened stream (fps)

    PLATFORM SUPPORT:
      Linux/V4L2  — true runtime change via VIDIOC_S_PARM (no restart needed).
      Windows     — NOT supported mid-stream.  SetFormat + Reconnect on a
                    running DirectShow graph cannot renegotiate timing while
                    pins are connected.  Close the stream and reopen with a
                    different format ID instead.
      macOS       — NOT supported (stub; returns CAPRESULT_ERR).

    returns: CAPRESULT_OK if the framerate was applied.
             CAPRESULT_ERR if not supported on this platform, or the
             stream cannot accept the requested framerate.
*/
DLLPUBLIC CapResult Cap_setFrameRate(CapContext ctx, CapStream stream, uint32_t fps);

/** get the value of a camera/stream property (e.g. zoom, exposure etc)

    returns: CAPRESULT_OK if all is well.
             CAPRESULT_PROPERTYNOTSUPPORTED if property not available.
             CAPRESULT_ERR if context, stream are invalid or outValue == NULL.
*/
DLLPUBLIC CapResult Cap_getProperty(CapContext ctx, CapStream stream, CapPropertyID propID, int32_t *outValue);

/** get the automatic flag of a camera/stream property (e.g. zoom, focus etc) 

    returns: CAPRESULT_OK if all is well.
             CAPRESULT_PROPERTYNOTSUPPORTED if property not available.
             CAPRESULT_ERR if context, stream are invalid.
*/
DLLPUBLIC CapResult Cap_getAutoProperty(CapContext ctx, CapStream stream, CapPropertyID propID, uint32_t *outValue);

/********************************************************************************** 
     DEBUGGING
**********************************************************************************/

/**
    Set the logging level.

    LOG LEVEL ID  | LEVEL 
    ------------- | -------------
    LOG_EMERG     | 0
    LOG_ALERT     | 1
    LOG_CRIT      | 2
    LOG_ERR       | 3
    LOG_WARNING   | 4
    LOG_NOTICE    | 5
    LOG_INFO      | 6    
    LOG_DEBUG     | 7
    LOG_VERBOSE   | 8

*/
DLLPUBLIC void Cap_setLogLevel(uint32_t level);


typedef void (*CapCustomLogFunc)(uint32_t level, const char *string);

/** install a custom callback for a logging function.

    the callback function must have the following 
    structure:

        void func(uint32_t level, const char *string);
*/
DLLPUBLIC void Cap_installCustomLogFunction(CapCustomLogFunc logFunc);

/** Return the version of the library as a string.
    In addition to a version number, this should 
    contain information on the platform,
    e.g. Win32/Win64/Linux32/Linux64/OSX etc,
    wether or not it is a release or debug
    build and the build date.

    When building the library, please set the 
    following defines in the build environment:

    __LIBVER__
    __PLATFORM__
    __BUILDTYPE__
    
*/

DLLPUBLIC const char* Cap_getLibraryVersion();

#endif
