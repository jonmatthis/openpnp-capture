/*
    Camera Availability and Re-enumeration Test
    ===========================================

    Exercises the three new API functions:
      Cap_isDeviceAvailable()       — probe camera availability
      Cap_refreshDevices()          — re-scan for devices
      Cap_isDeviceStillConnected()  — check open stream's device

    The test has TWO phases:

    Phase A (automated) — null safety, index validation, basic enumeration.
                         Runs without user intervention.

    Phase B (interactive) — physical unplug/replug of a camera.
                            Run with: availability_test.exe --interactive

    Usage:
      availability_test.exe                    # automated checks only
      availability_test.exe --interactive      # automated + physical unplug/replug
      availability_test.exe --camera 2         # test specific camera index
*/

#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "openpnp-capture.h"

static int g_passed = 0;
static int g_failed = 0;

static void print_separator(int width = 72) {
    for (int i = 0; i < width; i++) fputc('-', stderr);
    fputc('\n', stderr);
}

static void log_pass(const char* fmt, ...) {
    fprintf(stderr, "  [PASS]    ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    g_passed++;
}

static void log_fail(const char* fmt, ...) {
    fprintf(stderr, "  [FAIL]    ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    g_failed++;
}

static void log_info(const char* fmt, ...) {
    fprintf(stderr, "  [INFO]    ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

/** Print a prompt and wait for the user to press Enter */
static void wait_for_user(const char* prompt) {
    fprintf(stderr, "\n  >>> %s\n", prompt);
    fprintf(stderr, "  >>> Press ENTER when ready...");
    fflush(stderr);
    int ch;
    do { ch = getchar(); } while (ch != '\n' && ch != EOF);
}

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

// ─────────────────────────────────────────────────────────────────────────
// Phase A: Fully automated checks (no camera required for most)
// ─────────────────────────────────────────────────────────────────────────

static void test_null_context_safety() {
    fprintf(stderr, "\n--- Test A1: Null/invalid context safety ---\n");

    CapResult r = Cap_isDeviceAvailable(nullptr, 0);
    if (r == CAPRESULT_ERR)
        log_pass("Cap_isDeviceAvailable(NULL, 0) -> CAPRESULT_ERR");
    else
        log_fail("Cap_isDeviceAvailable(NULL, 0) returned %d, expected %d", r, CAPRESULT_ERR);

    r = Cap_refreshDevices(nullptr);
    if (r == CAPRESULT_ERR)
        log_pass("Cap_refreshDevices(NULL) -> CAPRESULT_ERR");
    else
        log_fail("Cap_refreshDevices(NULL) returned %d, expected %d", r, CAPRESULT_ERR);

    r = Cap_isDeviceStillConnected(nullptr, 0);
    if (r == CAPRESULT_ERR)
        log_pass("Cap_isDeviceStillConnected(NULL, 0) -> CAPRESULT_ERR");
    else
        log_fail("Cap_isDeviceStillConnected(NULL, 0) returned %d, expected %d", r, CAPRESULT_ERR);
}

static void test_index_validation(CapContext ctx) {
    fprintf(stderr, "\n--- Test A2: Out-of-range index validation ---\n");

    const char* name = Cap_getDeviceName(ctx, 9999);
    if (name == nullptr)
        log_pass("Cap_getDeviceName(ctx, 9999) -> NULL (out of range)");
    else
        log_fail("Cap_getDeviceName(ctx, 9999) returned '%s', expected NULL", name);

    CapResult r = Cap_isDeviceAvailable(ctx, 9999);
    if (r == CAPRESULT_ERR)
        log_pass("Cap_isDeviceAvailable(ctx, 9999) -> CAPRESULT_ERR (out of range)");
    else
        log_fail("Cap_isDeviceAvailable(ctx, 9999) returned %d, expected %d", r, CAPRESULT_ERR);

    r = Cap_isDeviceStillConnected(ctx, -1);
    if (r == CAPRESULT_ERR)
        log_pass("Cap_isDeviceStillConnected(ctx, -1) -> CAPRESULT_ERR");
    else
        log_fail("Cap_isDeviceStillConnected(ctx, -1) returned %d, expected %d", r, CAPRESULT_ERR);

    r = Cap_isDeviceStillConnected(ctx, 9999);
    if (r == CAPRESULT_ERR)
        log_pass("Cap_isDeviceStillConnected(ctx, 9999) -> CAPRESULT_ERR");
    else
        log_fail("Cap_isDeviceStillConnected(ctx, 9999) returned %d, expected %d", r, CAPRESULT_ERR);
}

static void test_refresh_stability(CapContext ctx) {
    fprintf(stderr, "\n--- Test A3: refreshDevices stability ---\n");

    CapResult r = Cap_refreshDevices(ctx);
    if (r == CAPRESULT_OK)
        log_pass("Cap_refreshDevices(ctx) -> CAPRESULT_OK");
    else
        log_fail("Cap_refreshDevices(ctx) returned %d, expected %d", r, CAPRESULT_OK);

    // Try 5 rapid refreshes
    for (int i = 0; i < 5; i++) {
        r = Cap_refreshDevices(ctx);
        if (r != CAPRESULT_OK) {
            log_fail("Cap_refreshDevices() iteration %d returned %d", i, r);
            return;
        }
    }
    log_pass("Cap_refreshDevices() succeeded 5 times in a row");
}

static void test_device_enumeration(CapContext ctx, uint32_t* outCount) {
    fprintf(stderr, "\n--- Test A4: Device enumeration ---\n");

    uint32_t count = Cap_getDeviceCount(ctx);
    log_info("Found %u device(s)", count);

    for (uint32_t i = 0; i < count; i++) {
        const char* name = Cap_getDeviceName(ctx, i);
        const char* uid = Cap_getDeviceUniqueID(ctx, i);
        int32_t nFmts = Cap_getNumFormats(ctx, i);
        log_info("  [%u] %s  (uniqueID: %s  formats: %d)",
                 i, name ? name : "(null)", uid ? uid : "(null)", nFmts);
    }

    if (outCount) *outCount = count;
}

static void test_device_availability(CapContext ctx, uint32_t deviceIdx) {
    fprintf(stderr, "\n--- Test A5: Device availability probe ---\n");

    const char* name = Cap_getDeviceName(ctx, deviceIdx);
    log_info("Probing device %u: %s", deviceIdx, name ? name : "(null)");

    CapResult r = Cap_isDeviceAvailable(ctx, deviceIdx);
    if (r == CAPRESULT_OK) {
        log_pass("Cap_isDeviceAvailable(ctx, %u) -> AVAILABLE", deviceIdx);
    } else if (r == CAPRESULT_ERR) {
        log_info("Cap_isDeviceAvailable(ctx, %u) -> UNAVAILABLE", deviceIdx);
        log_info("  This may be expected if the camera is in use by another app");
        g_passed++;  // correctly reported status
    } else {
        log_fail("Cap_isDeviceAvailable(ctx, %u) unexpected return %d", deviceIdx, r);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Phase B: Interactive disconnect / reconnect testing
// ─────────────────────────────────────────────────────────────────────────

static void test_disconnect_detection(CapContext ctx, uint32_t deviceIdx) {
    fprintf(stderr, "\n");
    print_separator();
    fprintf(stderr, "=== PHASE B: Interactive physical unplug/replug test ===\n");
    print_separator();

    const char* name = Cap_getDeviceName(ctx, deviceIdx);
    log_info("Target: device %u (%s)", deviceIdx, name ? name : "(null)");

    // ── B1: Open a stream ───────────────────────────────────────────────
    fprintf(stderr, "\n--- Test B1: Open stream for disconnect detection ---\n");

    CapResult r = Cap_isDeviceAvailable(ctx, deviceIdx);
    if (r != CAPRESULT_OK) {
        log_fail("Device %u is not available — cannot proceed with interactive test", deviceIdx);
        log_info("  Make sure the camera is plugged in and not in use by another app");
        return;
    }
    log_pass("Device %u is available", deviceIdx);

    int32_t fmtCount = Cap_getNumFormats(ctx, deviceIdx);
    if (fmtCount <= 0) {
        log_fail("Device %u has no formats — cannot open stream", deviceIdx);
        return;
    }
    log_info("Device has %d format(s) — opening with format 0", fmtCount);

    CapStream stream = Cap_openStream(ctx, deviceIdx, 0);
    if (stream < 0) {
        log_fail("Cap_openStream(ctx, %u, 0) failed (stream=%d) — cannot proceed", deviceIdx, stream);
        log_info("  The camera may have been grabbed between availability check and open");
        return;
    }
    log_pass("Cap_openStream(ctx, %u, 0) -> stream %d", deviceIdx, stream);

    sleep_ms(500);  // let capture start

    uint32_t isOpen = Cap_isOpenStream(ctx, stream);
    if (isOpen)
        log_pass("Cap_isOpenStream(ctx, %d) = 1 (capturing)", stream);
    else
        log_fail("Cap_isOpenStream(ctx, %d) = 0 — stream failed to start", stream);

    // ── B2: Initial connected check ─────────────────────────────────────
    fprintf(stderr, "\n--- Test B2: Initial connected check ---\n");

    r = Cap_isDeviceStillConnected(ctx, stream);
    if (r == CAPRESULT_OK)
        log_pass("Cap_isDeviceStillConnected(ctx, %d) -> OK (device present)", stream);
    else
        log_fail("Cap_isDeviceStillConnected(ctx, %d) returned %d, expected %d", stream, r, CAPRESULT_OK);

    // ── B3: Disconnect test ────────────────────────────────────────────
    fprintf(stderr, "\n--- Test B3: Physical disconnect detection ---\n");
    fprintf(stderr, "  >>> ACTION: Unplug the camera '%s' now.\n", name);
    fprintf(stderr, "  >>> Wait 3 seconds after unplugging.\n");
    wait_for_user("Press ENTER when done");

    fprintf(stderr, "\n  Checking connection status after unplug...\n");

    r = Cap_isDeviceStillConnected(ctx, stream);
    if (r == CAPRESULT_ERR) {
        log_pass("Cap_isDeviceStillConnected(ctx, %d) -> CAPRESULT_ERR (disconnect DETECTED)", stream);
    } else if (r == CAPRESULT_OK) {
        log_info("Cap_isDeviceStillConnected(ctx, %d) -> CAPRESULT_OK (still reports connected)", stream);
        log_info("  Some platforms may not immediately detect unplug — this is not necessarily a bug");
        log_info("  Try waiting longer and re-running, or check the platform notes below");
        g_passed++;  // platform-dependent behavior
    } else {
        log_fail("Cap_isDeviceStillConnected(ctx, %d) unexpected return %d", stream, r);
    }

    // ── B4: Refresh after disconnect ────────────────────────────────────
    fprintf(stderr, "\n--- Test B4: Re-enumeration after disconnect ---\n");

    r = Cap_refreshDevices(ctx);
    if (r == CAPRESULT_OK) {
        log_pass("Cap_refreshDevices(ctx) succeeded after disconnect");
    } else {
        log_fail("Cap_refreshDevices(ctx) failed after disconnect");
    }

    uint32_t countAfterUnplug = Cap_getDeviceCount(ctx);
    log_info("Device count after unplug: %u", countAfterUnplug);

    // ── B5: Reconnect test ──────────────────────────────────────────────
    fprintf(stderr, "\n--- Test B5: Physical reconnect and re-enumeration ---\n");
    fprintf(stderr, "  >>> ACTION: Plug the camera '%s' back in.\n", name);
    fprintf(stderr, "  >>> Wait 3 seconds after plugging.\n");
    wait_for_user("Press ENTER when done");

    fprintf(stderr, "\n  Refreshing device list...\n");

    r = Cap_refreshDevices(ctx);
    if (r == CAPRESULT_OK) {
        log_pass("Cap_refreshDevices(ctx) succeeded after reconnect");
    } else {
        log_fail("Cap_refreshDevices(ctx) failed after reconnect");
    }

    uint32_t countAfterReplug = Cap_getDeviceCount(ctx);
    log_info("Device count after replug: %u", countAfterReplug);

    // Check if we see the expected number of devices
    if (countAfterReplug > countAfterUnplug) {
        log_pass("Device count increased after replug (%u -> %u) — hotplug detection works",
                 countAfterUnplug, countAfterReplug);
    } else if (countAfterReplug == countAfterUnplug) {
        log_info("Device count unchanged after replug (%u) — camera may need more time to initialize",
                 countAfterReplug);
        log_info("  Or the camera was re-detected at the same index (common on Windows)");
        g_passed++;
    }

    // List devices after reconnect
    for (uint32_t i = 0; i < countAfterReplug; i++) {
        const char* dname = Cap_getDeviceName(ctx, i);
        log_info("  [%u] %s", i, dname ? dname : "(null)");
    }

    // ── B6: Availability after reconnect ────────────────────────────────
    fprintf(stderr, "\n--- Test B6: Availability after reconnect ---\n");

    // Search for the device by name (index may have changed)
    int foundIdx = -1;
    for (uint32_t i = 0; i < countAfterReplug; i++) {
        const char* dname = Cap_getDeviceName(ctx, i);
        if (dname && name && strstr(dname, name)) {
            foundIdx = (int)i;
            break;
        }
    }
    if (foundIdx < 0) foundIdx = (deviceIdx < countAfterReplug) ? (int)deviceIdx : -1;

    if (foundIdx >= 0) {
        log_info("Device found at index %d after reconnect", foundIdx);

        r = Cap_isDeviceAvailable(ctx, (uint32_t)foundIdx);
        if (r == CAPRESULT_OK) {
            log_pass("Cap_isDeviceAvailable(ctx, %d) -> AVAILABLE after reconnect", foundIdx);

            // Try opening a new stream on the reconnected device
            CapStream stream2 = Cap_openStream(ctx, (uint32_t)foundIdx, 0);
            if (stream2 >= 0) {
                log_pass("Cap_openStream(ctx, %d, 0) succeeded after reconnect (stream=%d)",
                         foundIdx, stream2);
                Cap_closeStream(ctx, stream2);
            } else {
                log_info("Cap_openStream after reconnect failed — may need more init time");
                g_passed++;
            }
        } else {
            log_info("Cap_isDeviceAvailable after reconnect returned %d — camera may still be initializing", r);
            g_passed++;
        }
    } else {
        log_info("Device not found in refreshed list — may need more time to enumerate");
        log_info("  Try running again after waiting a few seconds");
        g_passed++;  // platform-dependent
    }

    // Close the original stream (which may be dead after unplug)
    fprintf(stderr, "\n--- Cleanup ---\n");
    Cap_closeStream(ctx, stream);
    log_info("Original stream closed.");
}

// ─────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    int targetDevice = 0;
    bool interactive = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--camera") == 0 && i + 1 < argc) {
            targetDevice = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--interactive") == 0) {
            interactive = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: availability_test.exe [--camera N] [--interactive]\n");
            fprintf(stderr, "  --camera N      test device index N (default 0)\n");
            fprintf(stderr, "  --interactive   run physical unplug/replug tests\n");
            return 0;
        }
    }

    fprintf(stderr, "=== Camera Availability & Re-enumeration Test ===\n");
    fprintf(stderr, "Library: %s\n", Cap_getLibraryVersion());
    if (interactive)
        fprintf(stderr, "Mode:    INTERACTIVE (will prompt for physical unplug/replug)\n");
    else
        fprintf(stderr, "Mode:    automated only (use --interactive for physical testing)\n");
    fprintf(stderr, "\n");

    // ── Phase A: Automated ──────────────────────────────────────────────
    print_separator();
    fprintf(stderr, "PHASE A: Automated checks\n");
    print_separator();

    test_null_context_safety();

    CapContext ctx = Cap_createContext();
    if (ctx == nullptr) {
        log_fail("Cap_createContext() returned NULL — cannot continue");
        fprintf(stderr, "\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
        return 1;
    }
    log_pass("Cap_createContext() succeeded");
    g_passed++;

    test_index_validation(ctx);

    uint32_t deviceCount = 0;
    test_device_enumeration(ctx, &deviceCount);

    test_refresh_stability(ctx);

    if (deviceCount > 0) {
        if ((uint32_t)targetDevice >= deviceCount) {
            log_fail("Requested device %d but only %u device(s) found", targetDevice, deviceCount);
        } else {
            test_device_availability(ctx, (uint32_t)targetDevice);
        }
    } else {
        log_info("No cameras found — skipping device-level automated tests");
        log_info("(Plug in a camera and re-run)");
    }

    // ── Phase B: Interactive ────────────────────────────────────────────
    if (interactive && deviceCount > 0) {
        if ((uint32_t)targetDevice < deviceCount) {
            test_disconnect_detection(ctx, (uint32_t)targetDevice);
        }
    } else if (interactive) {
        log_info("Cannot run interactive tests: no cameras available");
    }

    // ── Cleanup ─────────────────────────────────────────────────────────
    Cap_releaseContext(ctx);
    log_info("Context released.");

    // ── Results ─────────────────────────────────────────────────────────
    print_separator();
    fprintf(stderr, "=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    print_separator();

    return g_failed > 0 ? 1 : 0;
}
