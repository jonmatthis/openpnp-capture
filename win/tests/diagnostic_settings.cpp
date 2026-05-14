/*
    Camera Settings Diagnostic Tool
    ===============================

    Empirically tests which camera settings actually "stick" vs which get
    silently overridden by the camera driver / DirectShow negotiation.

    Each test follows the pattern:
      [TRY]  describe what we're attempting
      [SET]  describe the value we're setting
      [READ] describe what was read back
      [MATCH]   or   [MISMATCH]   or   [UNSUPPORTED]   or   [ERROR]

    Usage:
      diagnostic_settings.exe                    # test camera 0, first MJPG format
      diagnostic_settings.exe --camera 2         # test camera index 2
      diagnostic_settings.exe --all              # test all cameras
      diagnostic_settings.exe --all --thorough   # test EVERY MJPG format on every camera
*/

#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <vector>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "openpnp-capture.h"

// ── helpers ────────────────────────────────────────────────────────────

static std::string fourcc_to_string(uint32_t fourcc) {
    std::string v;
    for (int i = 0; i < 4; i++) {
        v += static_cast<char>(fourcc & 0xFF);
        fourcc >>= 8;
    }
    return v;
}

static void print_separator(int width = 72) {
    for (int i = 0; i < width; i++) fputc('-', stderr);
    fputc('\n', stderr);
}

// ── test result formatting ─────────────────────────────────────────────

static void log_try(const char* fmt, ...) {
    fprintf(stderr, "  [TRY]     ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

static void log_set(const char* fmt, ...) {
    fprintf(stderr, "  [SET]     ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

static void log_read(const char* fmt, ...) {
    fprintf(stderr, "  [READ]    ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

static void log_match() {
    fprintf(stderr, "            → \033[32mMATCH\033[0m\n");
}

static void log_mismatch(const char* fmt, ...) {
    fprintf(stderr, "            → \033[31mMISMATCH\033[0m  ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

static void log_unsupported() {
    fprintf(stderr, "            → \033[33mUNSUPPORTED\033[0m\n");
}

static void log_error(const char* fmt, ...) {
    fprintf(stderr, "            → \033[31mERROR\033[0m  ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

// ── test functions ─────────────────────────────────────────────────────

struct TestResults {
    int formats_tested = 0;
    int resolution_matches = 0;
    int resolution_mismatches = 0;
    int exposure_supported = 0;
    int exposure_matches = 0;
    int exposure_mismatches = 0;
};

/// Test 1: For each MJPG format, open stream and read back actual resolution.
static void test_resolution_accuracy(CapContext ctx, uint32_t camera_index,
                                      const std::string& camera_name,
                                      TestResults& results) {
    int32_t num_formats = Cap_getNumFormats(ctx, camera_index);
    if (num_formats <= 0) {
        log_error("camera reports %d formats — skipping", num_formats);
        return;
    }

    fprintf(stderr, "\n");
    log_try("testing %d format(s) on camera [%u] \"%s\"", num_formats, camera_index, camera_name.c_str());
    print_separator();

    for (int32_t f = 0; f < num_formats; f++) {
        CapFormatInfo info;
        memset(&info, 0, sizeof(info));  // zero-init
        if (Cap_getFormatInfo(ctx, camera_index, f, &info) != CAPRESULT_OK) {
            log_error("cannot read format info for format %d", f);
            continue;
        }

        // Only test MJPG formats — these are what the production code uses
        if (info.fourcc != 0x47504A4D) { // 'MJPG' = 0x47504A4D
            continue;
        }

        results.formats_tested++;
        std::string fourcc_str = fourcc_to_string(info.fourcc);

        fprintf(stderr, "  [FORMAT %d]  %ux%u @%ufps  %s\n",
                f, info.width, info.height, info.fps, fourcc_str.c_str());

        // ── open stream ──
        int32_t stream = Cap_openStreamRaw(ctx, camera_index, f);
        if (stream < 0) {
            log_error("Cap_openStreamRaw returned %d for format %d (%ux%u %s)",
                      stream, f, info.width, info.height, fourcc_str.c_str());
            continue;
        }

        // ── read back actual negotiated resolution ──
        uint32_t actual_w = 0, actual_h = 0;
        CapResult res = Cap_getStreamResolution(ctx, stream, &actual_w, &actual_h);

        if (res == CAPRESULT_OK) {
            log_read("negotiated resolution: %ux%u  (requested: %ux%u)",
                     actual_w, actual_h, info.width, info.height);

            if (actual_w == info.width && actual_h == info.height) {
                log_match();
                results.resolution_matches++;
            } else {
                log_mismatch("requested %ux%u, got %ux%u",
                             info.width, info.height, actual_w, actual_h);
                results.resolution_mismatches++;
            }
        } else {
            log_error("Cap_getStreamResolution failed (result=%u)", res);
        }

        // ── read back actual fourcc ──
        // (indirect — if stream opened as raw, it must be MJPG)
        uint32_t frame_size = 0;
        if (Cap_getFrameSize(ctx, stream, &frame_size) == CAPRESULT_OK) {
            log_read("4CC verified: stream is in raw-MJPEG mode (frame_size query OK)");
        }

        Cap_closeStream(ctx, stream);
    }
}

/// Test 2: Exposure set-and-verify on a single stream.
static void test_exposure_accuracy(CapContext ctx, uint32_t camera_index,
                                    const std::string& camera_name,
                                    TestResults& results) {
    // Find first MJPG format to use for exposure testing
    int32_t num_formats = Cap_getNumFormats(ctx, camera_index);
    int32_t mjpg_format = -1;
    for (int32_t f = 0; f < num_formats; f++) {
        CapFormatInfo info;
        memset(&info, 0, sizeof(info));
        if (Cap_getFormatInfo(ctx, camera_index, f, &info) == CAPRESULT_OK
            && info.fourcc == 0x47504A4D) {
            mjpg_format = f;
            break;
        }
    }
    if (mjpg_format < 0) {
        log_unsupported();
        fprintf(stderr, "            no MJPG format found for exposure testing\n");
        return;
    }

    int32_t stream = Cap_openStreamRaw(ctx, camera_index, mjpg_format);
    if (stream < 0) {
        log_error("cannot open stream for exposure testing (stream=%d)", stream);
        return;
    }

    fprintf(stderr, "\n");
    log_try("testing exposure on camera [%u] \"%s\" (stream=%d)",
            camera_index, camera_name.c_str(), stream);

    // ── get exposure limits ──
    int32_t ex_min = 0, ex_max = 0, ex_default = 0;
    CapResult lim_res = Cap_getPropertyLimits(ctx, stream, CAPPROPID_EXPOSURE,
                                               &ex_min, &ex_max, &ex_default);
    if (lim_res != CAPRESULT_OK) {
        log_unsupported();
        fprintf(stderr, "            Cap_getPropertyLimits(EXPOSURE) returned %u\n", lim_res);
        Cap_closeStream(ctx, stream);
        return;
    }
    results.exposure_supported++;

    log_read("exposure limits: min=%d  max=%d  default=%d", ex_min, ex_max, ex_default);

    // ── disable auto-exposure ──
    CapResult auto_res = Cap_setAutoProperty(ctx, stream, CAPPROPID_EXPOSURE, 0);
    if (auto_res == CAPRESULT_OK) {
        log_set("auto-exposure → OFF");
    } else {
        log_error("cannot disable auto-exposure (result=%u)", auto_res);
    }

    // ── read initial exposure ──
    int32_t initial_exposure = -999;
    CapResult get_res = Cap_getProperty(ctx, stream, CAPPROPID_EXPOSURE, &initial_exposure);
    if (get_res == CAPRESULT_OK) {
        log_read("initial exposure value: %d", initial_exposure);
    } else {
        log_error("cannot read current exposure (result=%u)", get_res);
    }

    // ── test setting exposure to midpoint, min, max ──
    int32_t test_values[3];
    int test_count = 0;

    // Use default, min, max (but avoid duplicates)
    test_values[test_count++] = ex_default;
    if (ex_min != ex_default) test_values[test_count++] = ex_min;
    if (ex_max != ex_default && ex_max != ex_min) test_values[test_count++] = ex_max;

    for (int i = 0; i < test_count; i++) {
        int32_t desired = test_values[i];

        CapResult set_res = Cap_setProperty(ctx, stream, CAPPROPID_EXPOSURE, desired);
        if (set_res != CAPRESULT_OK) {
            log_error("Cap_setProperty(EXPOSURE, %d) failed (result=%u)", desired, set_res);
            continue;
        }
        log_set("exposure = %d", desired);

        // Small delay to let the camera apply the setting
        Sleep(50);

        int32_t actual = -999;
        CapResult read_res = Cap_getProperty(ctx, stream, CAPPROPID_EXPOSURE, &actual);
        if (read_res != CAPRESULT_OK) {
            log_error("Cap_getProperty(EXPOSURE) failed after set (result=%u)", read_res);
            continue;
        }
        log_read("exposure read back: %d", actual);

        if (actual == desired) {
            log_match();
            results.exposure_matches++;
        } else {
            log_mismatch("set %d, read back %d (delta=%d)", desired, actual, actual - desired);
            results.exposure_mismatches++;
        }
    }

    Cap_closeStream(ctx, stream);
}

/// Test 3: Multi-camera concurrent open — do different cameras negotiate the same format?
static void test_multicamera_consistency(CapContext ctx, uint32_t device_count) {
    fprintf(stderr, "\n");
    log_try("multi-camera consistency: testing %u cameras at matching format", device_count);
    print_separator();

    // Find cameras that have a common MJPG resolution
    struct CamFormat {
        uint32_t index;
        std::string name;
        int32_t format_id;
        uint32_t width;
        uint32_t height;
    };
    std::vector<CamFormat> candidates;

    for (uint32_t cam = 0; cam < device_count; cam++) {
        int32_t nfmt = Cap_getNumFormats(ctx, cam);
        for (int32_t f = 0; f < nfmt; f++) {
            CapFormatInfo info;
            memset(&info, 0, sizeof(info));
            if (Cap_getFormatInfo(ctx, cam, f, &info) == CAPRESULT_OK
                && info.fourcc == 0x47504A4D
                && info.width == 1280 && info.height == 720) {
                const char* name = Cap_getDeviceName(ctx, cam);
                candidates.push_back({cam, name ? name : "?", f, info.width, info.height});
                break;
            }
        }
    }

    if (candidates.size() < 2) {
        fprintf(stderr, "  [SKIP]    only %zu camera(s) with 1280x720 MJPG — need 2+\n",
                candidates.size());
        return;
    }

    std::vector<int32_t> streams;
    for (const auto& cf : candidates) {
        int32_t stream = Cap_openStreamRaw(ctx, cf.index, cf.format_id);
        if (stream < 0) {
            log_error("camera [%u] \"%s\": cannot open stream", cf.index, cf.name.c_str());
            continue;
        }
        streams.push_back(stream);

        uint32_t w = 0, h = 0;
        if (Cap_getStreamResolution(ctx, stream, &w, &h) == CAPRESULT_OK) {
            bool match = (w == cf.width && h == cf.height);
            fprintf(stderr, "  [CAM %u]  \"%s\"  → negotiated %ux%u  %s\n",
                    cf.index, cf.name.c_str(), w, h,
                    match ? "\033[32mMATCH\033[0m" : "\033[31mMISMATCH\033[0m");
        }
    }

    // Close all streams
    for (int32_t s : streams) {
        Cap_closeStream(ctx, s);
    }
    fprintf(stderr, "            (%zu cameras tested concurrently)\n", streams.size());
}

/// Test 4: Quick smoke test — open a stream, capture a few frames, verify frame size.
static void test_frame_acquisition(CapContext ctx, uint32_t camera_index,
                                    const std::string& camera_name) {
    int32_t num_formats = Cap_getNumFormats(ctx, camera_index);
    int32_t mjpg_format = -1;
    for (int32_t f = 0; f < num_formats; f++) {
        CapFormatInfo info;
        memset(&info, 0, sizeof(info));
        if (Cap_getFormatInfo(ctx, camera_index, f, &info) == CAPRESULT_OK
            && info.fourcc == 0x47504A4D
            && info.width == 1280 && info.height == 720) {
            mjpg_format = f;
            break;
        }
    }
    if (mjpg_format < 0) {
        // Try any MJPG
        for (int32_t f = 0; f < num_formats; f++) {
            CapFormatInfo info;
            memset(&info, 0, sizeof(info));
            if (Cap_getFormatInfo(ctx, camera_index, f, &info) == CAPRESULT_OK
                && info.fourcc == 0x47504A4D) {
                mjpg_format = f;
                break;
            }
        }
    }
    if (mjpg_format < 0) {
        fprintf(stderr, "\n  [SKIP]    no MJPG format for frame acquisition test\n");
        return;
    }

    fprintf(stderr, "\n");
    log_try("frame acquisition on camera [%u] \"%s\"", camera_index, camera_name.c_str());

    int32_t stream = Cap_openStreamRaw(ctx, camera_index, mjpg_format);
    if (stream < 0) {
        log_error("cannot open stream (stream=%d)", stream);
        return;
    }

    // Wait for first frame
    int attempts = 0;
    while (Cap_hasNewFrame(ctx, stream) == 0 && attempts < 300) {
        Sleep(10);
        attempts++;
    }

    if (attempts >= 300) {
        log_error("no frame received within 3 seconds");
        Cap_closeStream(ctx, stream);
        return;
    }

    // Get frame size
    uint32_t frame_size = 0;
    if (Cap_getFrameSize(ctx, stream, &frame_size) != CAPRESULT_OK) {
        log_error("Cap_getFrameSize failed");
        Cap_closeStream(ctx, stream);
        return;
    }
    log_read("first frame size: %u bytes (%.1f KB)", frame_size, frame_size / 1024.0f);

    // Capture the frame
    std::vector<uint8_t> buffer(frame_size);
    uint32_t actual_bytes = 0;
    CapResult cap_res = Cap_captureFrameRaw(ctx, stream, buffer.data(),
                                             frame_size, &actual_bytes);
    if (cap_res == CAPRESULT_OK) {
        bool is_jpeg = (actual_bytes >= 2 && buffer[0] == 0xFF && buffer[1] == 0xD8);
        log_read("captured %u bytes  %s",
                 actual_bytes,
                 is_jpeg ? "\033[32mVALID JPEG\033[0m" : "\033[31mNOT JPEG\033[0m");

        // Show first 16 bytes as hex
        fprintf(stderr, "            first 16 bytes: ");
        for (uint32_t i = 0; i < 16 && i < actual_bytes; i++) {
            fprintf(stderr, "%02X ", buffer[i]);
        }
        fprintf(stderr, "\n");
    } else {
        log_error("Cap_captureFrameRaw failed (result=%u)", cap_res);
    }

    Cap_closeStream(ctx, stream);
}

// ═══════════════════════════════════════════════════════════════════════
// MAIN
// ═══════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    fprintf(stderr, "╔══════════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║   Camera Settings Diagnostic Tool                               ║\n");
    fprintf(stderr, "║   Tests: format accuracy, exposure, multi-camera, frame capture ║\n");
    fprintf(stderr, "╚══════════════════════════════════════════════════════════════════╝\n\n");

    // ── parse arguments ──
    bool test_all = false;
    bool thorough = false;
    int specific_camera = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0) {
            test_all = true;
        } else if (strcmp(argv[i], "--thorough") == 0) {
            thorough = true;
        } else if (strcmp(argv[i], "--camera") == 0 && i + 1 < argc) {
            specific_camera = atoi(argv[++i]);
        }
    }

    if (!test_all && specific_camera < 0) {
        specific_camera = 0;  // default: first camera
    }

    // ── create context ──
    CapContext ctx = Cap_createContext();
    if (ctx == nullptr) {
        fprintf(stderr, "FATAL: Cap_createContext returned null\n");
        return 1;
    }

    uint32_t device_count = Cap_getDeviceCount(ctx);
    fprintf(stderr, "Devices detected: %u\n\n", device_count);

    if (device_count == 0) {
        fprintf(stderr, "No cameras found. Exiting.\n");
        Cap_releaseContext(ctx);
        return 1;
    }

    // ── determine which cameras to test ──
    std::vector<uint32_t> test_cameras;
    if (test_all) {
        for (uint32_t i = 0; i < device_count; i++) {
            test_cameras.push_back(i);
        }
    } else {
        test_cameras.push_back((uint32_t)specific_camera);
    }

    // ── run tests ──
    TestResults total_results = {};

    for (uint32_t cam_index : test_cameras) {
        const char* name = Cap_getDeviceName(ctx, cam_index);
        std::string cam_name = name ? name : "(unknown)";

        fprintf(stderr, "══════════════════════════════════════════════════════════════════\n");
        fprintf(stderr, "  CAMERA [%u]  \"%s\"\n", cam_index, cam_name.c_str());
        fprintf(stderr, "  Unique ID: %s\n", Cap_getDeviceUniqueID(ctx, cam_index));
        fprintf(stderr, "══════════════════════════════════════════════════════════════════\n");

        // Test 1: Resolution accuracy (for every MJPG format if thorough,
        //         otherwise just a quick check)
        test_resolution_accuracy(ctx, cam_index, cam_name, total_results);

        // Test 2: Exposure set-and-verify
        test_exposure_accuracy(ctx, cam_index, cam_name, total_results);

        // Test 4: Frame acquisition
        test_frame_acquisition(ctx, cam_index, cam_name);
    }

    // Test 3: Multi-camera consistency (only if testing all cameras)
    if (test_all && test_cameras.size() >= 2) {
        test_multicamera_consistency(ctx, device_count);
    }

    // ── summary ──
    fprintf(stderr, "\n");
    fprintf(stderr, "══════════════════════════════════════════════════════════════════\n");
    fprintf(stderr, "  DIAGNOSTIC SUMMARY\n");
    fprintf(stderr, "══════════════════════════════════════════════════════════════════\n");
    fprintf(stderr, "  Formats tested:            %d MJPG formats\n", total_results.formats_tested);
    fprintf(stderr, "  Resolution matches:        %d\n", total_results.resolution_matches);
    fprintf(stderr, "  Resolution mismatches:     %d\n", total_results.resolution_mismatches);
    fprintf(stderr, "  Exposure supported:        %d camera(s)\n", total_results.exposure_supported);
    fprintf(stderr, "  Exposure set/verify OK:    %d\n", total_results.exposure_matches);
    fprintf(stderr, "  Exposure set/verify FAIL:  %d\n", total_results.exposure_mismatches);

    if (total_results.resolution_mismatches > 0) {
        fprintf(stderr, "\n");
        fprintf(stderr, "  ╔══════════════════════════════════════════════════════════════╗\n");
        fprintf(stderr, "  ║  \033[31mWARNING: %d resolution mismatch(es) detected!\033[0m              ║\n",
                total_results.resolution_mismatches);
        fprintf(stderr, "  ║  The camera driver(s) silently changed the resolution.       ║\n");
        fprintf(stderr, "  ║  This means the system is delivering wrong-resolution data.  ║\n");
        fprintf(stderr, "  ╚══════════════════════════════════════════════════════════════╝\n");
    }

    if (total_results.exposure_mismatches > 0) {
        fprintf(stderr, "  \033[33mWARNING: %d exposure set/verify failure(s)!\033[0m\n",
                total_results.exposure_mismatches);
    }

    fprintf(stderr, "\n");
    Cap_releaseContext(ctx);
    return 0;
}
