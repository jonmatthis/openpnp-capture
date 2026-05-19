/*
    Camera Availability Test — Quiet Probe vs Definitive Check
    ==========================================================

    Demonstrates the TWO tiers of availability detection:

    TIER 1 — Quiet probe  (Cap_isDeviceAvailable)
             Fast (~ms), non-invasive, no sensor power-on.
             Checks device existence and basic accessibility.

    TIER 2 — Definitive check  (Cap_openStream + capture frame + close)
             Slower (~500ms+), powers on sensor, turns on LED.
             Proves the camera can actually deliver frames.

    The key insight: a camera can pass Tier 1 but fail Tier 2 if another
    app has an exclusive lock (e.g. Windows Camera via WinRT while OBS
    uses DirectShow sharing).

    Usage:
      inuse_test.exe                          # scan all cameras, both tiers
      inuse_test.exe --camera 2               # test only camera index 2
      inuse_test.exe --interactive            # also do OBS/WinCamera in-use test
*/

#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#else
#include <unistd.h>
#endif

#include "openpnp-capture.h"

static int g_passed = 0;
static int g_failed = 0;

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static void print_sep(char ch, int w = 72) {
    for (int i = 0; i < w; i++) fputc(ch, stderr);
    fputc('\n', stderr);
}

static void log_section(const char* fmt, ...) {
    fprintf(stderr, "\n");
    print_sep('=');
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    print_sep('=');
}

static void log_pass(const char* fmt, ...) {
    fprintf(stderr, "  [PASS]    ");
    va_list args; va_start(args, fmt);
    vfprintf(stderr, fmt, args); va_end(args);
    fputc('\n', stderr); g_passed++;
}

static void log_fail(const char* fmt, ...) {
    fprintf(stderr, "  [FAIL]    ");
    va_list args; va_start(args, fmt);
    vfprintf(stderr, fmt, args); va_end(args);
    fputc('\n', stderr); g_failed++;
}

static void log_info(const char* fmt, ...) {
    fprintf(stderr, "  [INFO]    ");
    va_list args; va_start(args, fmt);
    vfprintf(stderr, fmt, args); va_end(args);
    fputc('\n', stderr);
}

static void wait_for_user(const char* prompt) {
    fprintf(stderr, "\n  >>> %s\n", prompt);
    fprintf(stderr, "  >>> Press ENTER when ready...");
    fflush(stderr);
    int ch;
    do { ch = getchar(); } while (ch != '\n' && ch != EOF);
}

static std::string short_uid(const char* fullUID) {
    if (fullUID == nullptr) return "(null)";
    std::string s(fullUID);
    size_t usbPos = s.find("usb#");
    if (usbPos != std::string::npos) {
        s = s.substr(usbPos);
    } else {
        size_t slash = s.find("\\\\");
        if (slash != std::string::npos) s = s.substr(slash);
    }
    size_t guidStart = s.find("{65e8773d");
    if (guidStart != std::string::npos && guidStart > 0) {
        s = s.substr(0, guidStart - 1);
    }
    size_t global = s.find("\\global");
    if (global != std::string::npos) s = s.substr(0, global);
    return s;
}

// ─────────────────────────────────────────────────────────────────────────
// TIER 1: Quiet probe — just Cap_isDeviceAvailable()
// ─────────────────────────────────────────────────────────────────────────

struct Tier1Result {
    uint32_t    index;
    std::string name;
    std::string uidShort;
    int32_t     nFormats;
    bool        passed;
    CapResult   resultCode;
};

static std::vector<Tier1Result> run_quiet_probe(CapContext ctx) {
    std::vector<Tier1Result> results;
    uint32_t count = Cap_getDeviceCount(ctx);

    for (uint32_t i = 0; i < count; i++) {
        Tier1Result r;
        r.index     = i;
        r.name      = Cap_getDeviceName(ctx, i) ? Cap_getDeviceName(ctx, i) : "(null)";
        r.uidShort  = short_uid(Cap_getDeviceUniqueID(ctx, i));
        r.nFormats  = Cap_getNumFormats(ctx, i);
        r.resultCode = Cap_isDeviceAvailable(ctx, i);
        r.passed    = (r.resultCode == CAPRESULT_OK);
        results.push_back(r);
    }
    return results;
}

static void print_tier1_table(const std::vector<Tier1Result>& results) {
    fprintf(stderr, "\n");
    print_sep('-');
    fprintf(stderr, "  TIER 1 — Quiet probe (Cap_isDeviceAvailable)\n");
    fprintf(stderr, "  Fast (~ms). Does NOT power on sensor or capture frames.\n");
    print_sep('-');

    if (results.empty()) {
        fprintf(stderr, "  (no cameras found)\n");
        return;
    }

    fprintf(stderr, "  %3s  %-18s  %4s  %-8s  %s\n",
            "IDX", "NAME", "FMTS", "PROBE", "UNIQUE ID");
    fprintf(stderr, "  %3s  %-18s  %4s  %-8s  %s\n",
            "---", "------------------", "----", "--------", "---------");

    int nPassed = 0;
    for (size_t i = 0; i < results.size(); i++) {
        const Tier1Result& r = results[i];
        const char* status = r.passed ? "FREE" : "UNAVAIL";
        nPassed += r.passed ? 1 : 0;
        fprintf(stderr, "  %3u  %-18s  %4d  %-8s  %s\n",
                r.index,
                r.name.c_str(),
                r.nFormats,
                status,
                r.uidShort.c_str());
    }
    print_sep('-');
    fprintf(stderr, "  Quiet probe: %d of %zu camera(s) report FREE\n",
            nPassed, results.size());

    if (nPassed < (int)results.size()) {
        fprintf(stderr, "  %d camera(s) show UNAVAIL — these may be virtual devices,\n",
                (int)results.size() - nPassed);
        fprintf(stderr, "  locked by another app, or have driver issues.\n");
    }
}

// ─────────────────────────────────────────────────────────────────────────
// TIER 2: Definitive check — actually open, capture frame, close
// ─────────────────────────────────────────────────────────────────────────

struct Tier2Result {
    uint32_t    index;
    bool        opened;         // Cap_openStream succeeded
    bool        gotFrame;       // Cap_hasNewFrame returned true
    uint32_t    openTimeMs;     // how long open() took
    uint32_t    frameWaitMs;    // how long until first frame
    std::string error;          // why it failed (if it did)
};

/** Per-thread worker: create its own context, open one camera, capture frame, close.
    Each thread gets its own CapContext so there's no shared-state contention. */
static void definitive_check_thread(uint32_t deviceIdx, Tier2Result* out) {
    Tier2Result r;
    r.index      = deviceIdx;
    r.opened     = false;
    r.gotFrame   = false;
    r.openTimeMs = 0;
    r.frameWaitMs = 0;

#ifdef _WIN32
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
#endif

    // Each thread creates its own context — avoids thread-safety issues
    CapContext ctx = Cap_createContext();
    if (ctx == nullptr) {
        r.error = "context creation failed";
        *out = r;
#ifdef _WIN32
        CoUninitialize();
#endif
        return;
    }

    uint32_t count = Cap_getDeviceCount(ctx);
    if (deviceIdx >= count) {
        r.error = "device index out of range after re-enumeration";
        Cap_releaseContext(ctx);
        *out = r;
#ifdef _WIN32
        CoUninitialize();
#endif
        return;
    }

    int32_t nFmts = Cap_getNumFormats(ctx, deviceIdx);
    if (nFmts <= 0) {
        r.error = "no formats";
        Cap_releaseContext(ctx);
        *out = r;
#ifdef _WIN32
        CoUninitialize();
#endif
        return;
    }

#ifdef _WIN32
    DWORD t0 = GetTickCount();
#endif

    CapStream stream = Cap_openStream(ctx, deviceIdx, 0);
    if (stream < 0) {
        r.error = "openStream failed";
        Cap_releaseContext(ctx);
        *out = r;
#ifdef _WIN32
        CoUninitialize();
#endif
        return;
    }

#ifdef _WIN32
    r.openTimeMs = GetTickCount() - t0;
#endif

    r.opened = true;

    // Wait for first frame (up to 2 seconds)
    int waited = 0;
    while (waited < 40) {
        sleep_ms(50);
        waited++;
        if (Cap_hasNewFrame(ctx, stream)) {
            r.gotFrame = true;
            break;
        }
    }

    r.frameWaitMs = waited * 50;

    if (!r.gotFrame) {
        r.error = "opened but no frame arrived in 2s";
    }

    Cap_closeStream(ctx, stream);
    Cap_releaseContext(ctx);

#ifdef _WIN32
    CoUninitialize();
#endif

    *out = r;
}

/** Run definitive checks on all cameras IN PARALLEL, one thread per camera. */
static std::vector<Tier2Result> run_definitive_check_parallel(
        const std::vector<uint32_t>& deviceIndices)
{
    size_t n = deviceIndices.size();
    std::vector<Tier2Result> results(n);
    std::vector<std::thread> threads;
    threads.reserve(n);

    for (size_t i = 0; i < n; i++) {
        threads.emplace_back(definitive_check_thread, deviceIndices[i], &results[i]);
    }

    for (auto& t : threads) {
        t.join();
    }

    return results;
}

static void print_tier2_table(const std::vector<Tier2Result>& results) {
    fprintf(stderr, "\n");
    print_sep('-');
    fprintf(stderr, "  TIER 2 — Definitive check (open → capture frame → close)\n");
    fprintf(stderr, "  Slower (~500ms+). Powers on sensor, turns on LED, captures real frame.\n");
    print_sep('-');

    fprintf(stderr, "  %3s  %8s  %9s  %10s  %s\n",
            "IDX", "OPENED?", "GOT FRAME?", "OPEN TIME", "ERROR / NOTES");
    fprintf(stderr, "  %3s  %8s  %9s  %10s  %s\n",
            "---", "--------", "---------", "----------", "-------------");

    int nOpened = 0, nGotFrame = 0;
    for (size_t i = 0; i < results.size(); i++) {
        const Tier2Result& r = results[i];
        char timeStr[32];
        if (r.opened) {
            snprintf(timeStr, sizeof(timeStr), "%u ms", r.openTimeMs);
        } else {
            snprintf(timeStr, sizeof(timeStr), "—");
        }

        const char* openedStr  = r.opened   ? "YES" : "NO";
        const char* frameStr   = r.gotFrame ? "YES" : "NO";
        const char* errorStr   = r.error.empty() ? "" : r.error.c_str();

        fprintf(stderr, "  %3u  %8s  %9s  %10s  %s\n",
                r.index, openedStr, frameStr, timeStr, errorStr);

        if (r.opened)   nOpened++;
        if (r.gotFrame) nGotFrame++;
    }
    print_sep('-');
    fprintf(stderr, "  Opened: %d/%zu   Got frame: %d/%zu\n",
            nOpened, results.size(), nGotFrame, results.size());
}

// ─────────────────────────────────────────────────────────────────────────
// Comparison table: Tier 1 vs Tier 2 side by side
// ─────────────────────────────────────────────────────────────────────────

struct CompareRow {
    uint32_t    index;
    std::string name;
    std::string uidShort;
    bool        tier1Free;      // quiet probe says available
    bool        tier2Opened;    // definitive check opened
    bool        tier2Frame;     // definitive check got a frame
    uint32_t    tier1TimeMs;
    uint32_t    tier2TimeMs;
};

static void print_comparison(const std::vector<Tier1Result>& t1,
                              const std::vector<Tier2Result>& t2) {
    fprintf(stderr, "\n");
    print_sep('=');
    fprintf(stderr, "  COMPARISON: Quiet probe vs Definitive check\n");
    print_sep('=');

    fprintf(stderr, "  %3s  %-16s  %-8s  %-8s  %-11s  %s\n",
            "IDX", "NAME", "TIER 1", "TIER 2", "TIER 2", "UNIQUE ID");
    fprintf(stderr, "  %3s  %-16s  %-8s  %-8s  %-11s  %s\n",
            "---", "----------------", "--------", "--------", "-----------", "---------");

    for (size_t i = 0; i < t1.size() && i < t2.size(); i++) {
        const char* t1Str = t1[i].passed ? "FREE" : "UNAVAIL";
        const char* t2Str;
        if (t2[i].gotFrame)       t2Str = "FRAMES OK";
        else if (t2[i].opened)    t2Str = "NO FRAMES";
        else if (t2[i].error == "no formats") t2Str = "NO FORMATS";
        else                      t2Str = "WONT OPEN";

        fprintf(stderr, "  %3u  %-16s  %-8s  %-8s  %-11s  %s\n",
                t1[i].index,
                t1[i].name.c_str(),
                t1Str,
                t2Str,
                t2[i].error.empty() ? "" : t2[i].error.c_str(),
                t1[i].uidShort.c_str());
    }
    print_sep('=');

    // Highlight discrepancies
    int quietFreeButCantStream = 0;
    int quietUnavailButStreams = 0;
    for (size_t i = 0; i < t1.size() && i < t2.size(); i++) {
        if (t1[i].passed && !t2[i].gotFrame) quietFreeButCantStream++;
        if (!t1[i].passed && t2[i].gotFrame) quietUnavailButStreams++;
    }

    if (quietFreeButCantStream > 0) {
        fprintf(stderr, "  >>> %d camera(s) passed quiet probe but failed definitive check.\n",
                quietFreeButCantStream);
        fprintf(stderr, "  >>> These may have exclusive locks invisible to DirectShow enumeration.\n");
    }
    if (quietUnavailButStreams > 0) {
        fprintf(stderr, "  >>> %d camera(s) failed quiet probe but succeeded definitive check.\n",
                quietUnavailButStreams);
        fprintf(stderr, "  >>> Possible false negative in quiet probe.\n");
    }
    if (quietFreeButCantStream == 0 && quietUnavailButStreams == 0) {
        fprintf(stderr, "  >>> Tier 1 and Tier 2 agree on all cameras.\n");
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Optional interactive in-use test
// ─────────────────────────────────────────────────────────────────────────

static void run_interactive_test(CapContext ctx, uint32_t count) {
    log_section("INTERACTIVE: Open a camera in another app, then re-scan");

    // Baseline comparison
    fprintf(stderr, "\n  BASELINE — all cameras should be free.\n");
    fprintf(stderr, "  (Close any apps using cameras before continuing)\n");
    wait_for_user("Ready to take baseline");

    Cap_refreshDevices(ctx);
    std::vector<Tier1Result> t1Before = run_quiet_probe(ctx);
    print_tier1_table(t1Before);

    // Pick one camera and do definitive check before
    fprintf(stderr, "\n  Running definitive check on ONE camera (pick one):\n");
    for (uint32_t i = 0; i < count; i++) {
        fprintf(stderr, "    [%u] %s\n", i,
                Cap_getDeviceName(ctx, i) ? Cap_getDeviceName(ctx, i) : "(null)");
    }
    wait_for_user("Note the index of the camera you will open in another app");

    // Open in other app
    wait_for_user("ACTION: Open that camera in OBS / Windows Camera / Zoom now.\n"
                  "  >>> Make sure it is actively streaming.");

    // Re-scan with quiet probe
    fprintf(stderr, "\n  Re-scanning (quiet probe) while other app has camera...\n");
    Cap_refreshDevices(ctx);
    std::vector<Tier1Result> t1During = run_quiet_probe(ctx);
    print_tier1_table(t1During);

    // Show what changed
    fprintf(stderr, "\n  Changes from baseline:\n");
    bool anyChange = false;
    for (size_t i = 0; i < t1Before.size() && i < t1During.size(); i++) {
        if (t1Before[i].passed != t1During[i].passed) {
            fprintf(stderr, "    Camera %zu: %s -> %s\n", i,
                    t1Before[i].passed ? "FREE" : "UNAVAIL",
                    t1During[i].passed ? "FREE" : "UNAVAIL");
            anyChange = true;
        }
    }
    if (!anyChange) {
        fprintf(stderr, "    No changes — quiet probe cannot see exclusive locks\n");
        fprintf(stderr, "    at the WinRT/Media Foundation layer.\n");
    } else {
        log_pass("Quiet probe detected a change after opening in other app");
    }

    // Now run definitive check on candidates while other app has camera
    fprintf(stderr, "\n  Running DEFINITIVE check while other app has camera...\n");
    {
        std::vector<uint32_t> cands;
        for (uint32_t i = 0; i < count && i < t1During.size(); i++) {
            if (t1Before[i].passed) cands.push_back(i);
        }
        if (!cands.empty()) {
            std::vector<Tier2Result> t2During = run_definitive_check_parallel(cands);
            for (auto& r : t2During) {
                if (!r.gotFrame) {
                    log_pass("Definitive check detected issue on camera %u: %s",
                             r.index, r.error.c_str());
                } else {
                    log_info("Camera %u: opened and got frames (driver shares OK)", r.index);
                }
            }
        }
    }

    // Close other app
    wait_for_user("ACTION: Close the camera in the other app.");

    // Final re-scan
    fprintf(stderr, "\n  Final re-scan after closing other app...\n");
    Cap_refreshDevices(ctx);
    std::vector<Tier1Result> t1After = run_quiet_probe(ctx);
    print_tier1_table(t1After);
}

// ─────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    int targetDevice = -1;
    bool interactive = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--camera") == 0 && i + 1 < argc) {
            targetDevice = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--interactive") == 0) {
            interactive = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: inuse_test.exe [--camera N] [--interactive]\n\n");
            fprintf(stderr, "  Demonstrates TWO tiers of camera availability detection:\n\n");
            fprintf(stderr, "  TIER 1 (quiet probe):  Cap_isDeviceAvailable()\n");
            fprintf(stderr, "    - Fast (~ms), non-invasive, no sensor power-on\n");
            fprintf(stderr, "    - Checks device existence and basic accessibility\n\n");
            fprintf(stderr, "  TIER 2 (definitive):   Cap_openStream + capture frame + close\n");
            fprintf(stderr, "    - Slower (~500ms+), powers on sensor, turns on LED\n");
            fprintf(stderr, "    - Proves the camera can actually deliver frames\n\n");
            fprintf(stderr, "  --camera N     Only test device index N\n");
            fprintf(stderr, "  --interactive  Also run OBS/WinCamera in-use test\n");
            return 0;
        }
    }

    fprintf(stderr, "=== Camera Availability: Quiet Probe vs Definitive Check ===\n");
    fprintf(stderr, "Library: %s\n", Cap_getLibraryVersion());
    fprintf(stderr, "\nTIER 1 = Cap_isDeviceAvailable()   — fast, non-invasive, ~ms\n");
    fprintf(stderr, "TIER 2 = open → capture frame → close — slow, proves real access, ~500ms+\n");

    CapContext ctx = Cap_createContext();
    if (ctx == nullptr) {
        log_fail("Cap_createContext() returned NULL");
        return 1;
    }
    log_pass("Context created");

    uint32_t count = Cap_getDeviceCount(ctx);
    if (count == 0) {
        log_info("No cameras found. Plug in a camera and re-run.");
        Cap_releaseContext(ctx);
        return 0;
    }
    log_info("Found %u device(s)", count);

    // ── Determine which cameras to test ──────────────────────────────────
    std::vector<uint32_t> targets;
    if (targetDevice >= 0) {
        if ((uint32_t)targetDevice >= count) {
            log_fail("Device %d out of range (max %u)", targetDevice, count);
            Cap_releaseContext(ctx);
            return 1;
        }
        targets.push_back((uint32_t)targetDevice);
        log_info("Testing only camera %d", targetDevice);
    } else {
        for (uint32_t i = 0; i < count; i++) targets.push_back(i);
        log_info("Testing all %u cameras", count);
    }

    // ── TIER 1: Quiet probe on ALL cameras ──────────────────────────────
    log_section("TIER 1: Quiet probe (Cap_isDeviceAvailable)");
    fprintf(stderr, "  Scanning all cameras — this is fast and non-invasive.\n");

    std::vector<Tier1Result> t1Results = run_quiet_probe(ctx);
    print_tier1_table(t1Results);

    // ── TIER 2: Definitive check on selected cameras (PARALLEL) ─────────
    log_section("TIER 2: Definitive check (open → frame → close)");
    fprintf(stderr, "  Opening %zu camera(s) IN PARALLEL — one thread per camera.\n",
            targets.size());
    fprintf(stderr, "  Sensors will power on simultaneously. Camera LEDs will flash.\n");
    fprintf(stderr, "  Total time ≈ slowest camera, not sum of all.\n");

#ifdef _WIN32
    DWORD t2Start = GetTickCount();
#endif
    std::vector<Tier2Result> t2Results = run_definitive_check_parallel(targets);
#ifdef _WIN32
    DWORD t2Elapsed = GetTickCount() - t2Start;
    fprintf(stderr, "  Tier 2 elapsed: %u ms (parallel, %zu cameras)\n", t2Elapsed, targets.size());
#endif

    // Build a T2 array indexed the same as T1 for comparison
    // (or just handle the subset case)
    std::vector<Tier2Result> t2Full;
    if (targetDevice >= 0) {
        // Single camera — fill with dummies for other indices
        t2Full.resize(count);
        t2Full[targetDevice] = t2Results[0];
    } else {
        t2Full = t2Results;
    }

    print_tier2_table(t2Results);  // show only the ones we tested

    // ── Comparison ──────────────────────────────────────────────────────
    if (targetDevice < 0 || t1Results.size() == t2Full.size()) {
        // Build comparison rows for indices that exist in both
        std::vector<Tier1Result> t1Sub;
        std::vector<Tier2Result> t2Sub;
        for (size_t i = 0; i < t1Results.size(); i++) {
            for (size_t j = 0; j < t2Results.size(); j++) {
                if (t2Results[j].index == t1Results[i].index) {
                    t1Sub.push_back(t1Results[i]);
                    t2Sub.push_back(t2Results[j]);
                    break;
                }
            }
        }
        if (!t1Sub.empty()) {
            print_comparison(t1Sub, t2Sub);
        }
    }

    // ── Interactive test ────────────────────────────────────────────────
    if (interactive) {
        run_interactive_test(ctx, count);
    }

    // ── Cleanup ─────────────────────────────────────────────────────────
    Cap_releaseContext(ctx);
    fprintf(stderr, "\nDone. %d passed, %d failed.\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
