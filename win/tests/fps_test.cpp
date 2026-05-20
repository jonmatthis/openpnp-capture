/*
    FPS Detection & Framerate Setting Test
    =======================================

    Phase 1 — Enumeration: lists every camera and ALL its formats with FPS.
    Phase 2 — Selection: picks unique camera models (one of each USB type
              plus any Arducam/OV9281) to avoid testing 5 identical cams.
    Phase 3 — FPS testing:
      a) Open a stream at a specific format/FPS, measure actual FPS.
      b) Call Cap_setFrameRate() to change FPS, measure again.
      c) Report whether the FPS change took effect.

    Usage:
      fps_test.exe                    # auto-select unique cameras
      fps_test.exe --all              # test every camera
      fps_test.exe --camera N         # test only camera index N
*/

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "openpnp-capture.h"

static std::string fourcc_to_string(uint32_t fourcc) {
    std::string v;
    for (int i = 0; i < 4; i++) {
        v += static_cast<char>(fourcc & 0xFF);
        fourcc >>= 8;
    }
    return v;
}

static void print_sep(int w = 72) {
    for (int i = 0; i < w; i++) fputc('-', stdout);
    fputc('\n', stdout);
}

static double measure_fps(CapContext ctx, int32_t stream) {
    // Drain any buffered frames
    for (int i = 0; i < 5; i++) {
        Sleep(50);
        Cap_hasNewFrame(ctx, stream);
    }

    uint32_t fstart = Cap_getStreamFrameCount(ctx, stream);
    Sleep(2000);
    uint32_t fend = Cap_getStreamFrameCount(ctx, stream);

    if (fend < fstart) return 0.0;
    return (fend - fstart) / 2.0;
}

struct CapFormat {
    uint32_t width, height, fourcc, fps, bpp;
    int32_t  id;
};

struct Camera {
    uint32_t index;
    std::string name;
    std::string unique_id;
    std::vector<CapFormat> formats;
};

int main(int argc, char* argv[]) {
    printf("============================================================\n");
    printf("  FPS Detection & Setting Test\n");
    printf("  %s\n", Cap_getLibraryVersion());
    printf("============================================================\n\n");

    bool test_all = false;
    int  specific_camera = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0) test_all = true;
        else if (strcmp(argv[i], "--camera") == 0 && i + 1 < argc)
            specific_camera = atoi(argv[++i]);
    }

    Cap_setLogLevel(6);
    CapContext ctx = Cap_createContext();
    if (!ctx) {
        fprintf(stderr, "FATAL: Cap_createContext returned null\n");
        return 1;
    }

    uint32_t ndev = Cap_getDeviceCount(ctx);
    printf("Devices detected: %u\n\n", ndev);
    if (ndev == 0) {
        printf("No cameras found.\n");
        Cap_releaseContext(ctx);
        return 1;
    }

    // ── Phase 1: Enumerate ──────────────────────────────────────────────
    std::vector<Camera> cameras;
    std::map<std::string, uint32_t> all_fps_values;  // across all cams

    for (uint32_t i = 0; i < ndev; i++) {
        Camera cam;
        cam.index = i;
        const char* s = Cap_getDeviceName(ctx, i);
        cam.name = s ? s : "(unknown)";
        s = Cap_getDeviceUniqueID(ctx, i);
        cam.unique_id = s ? s : "(unknown)";

        int32_t n = Cap_getNumFormats(ctx, i);
        printf("Camera %u: \"%s\"\n", i, cam.name.c_str());
        printf("  Unique ID: %s\n", cam.unique_id.c_str());
        printf("  Formats: %d\n", n);

        for (int32_t j = 0; j < n; j++) {
            CapFormatInfo info;
            memset(&info, 0, sizeof(info));
            if (Cap_getFormatInfo(ctx, i, j, &info) == CAPRESULT_OK) {
                CapFormat f;
                f.width  = info.width;
                f.height = info.height;
                f.fourcc = info.fourcc;
                f.fps    = info.fps;
                f.bpp    = info.bpp;
                f.id     = j;
                cam.formats.push_back(f);

                printf("    [%3d] %4ux%-4u  %-6s  @%3u fps  bpp=%u\n",
                       j, info.width, info.height,
                       fourcc_to_string(info.fourcc).c_str(),
                       info.fps, info.bpp);

                if (info.fps > 0) all_fps_values[fourcc_to_string(info.fourcc)] = 1;
            }
        }
        printf("\n");
        cameras.push_back(cam);
    }

    printf("FOURCC values seen across all cameras:");
    for (const auto& kv : all_fps_values) {
        printf(" %s", kv.first.c_str());
    }
    printf("\n\n");

    // ── Phase 2: Select unique cameras ───────────────────────────────────
    std::vector<uint32_t> to_test;

    if (specific_camera >= 0) {
        if (specific_camera < (int)ndev) to_test.push_back((uint32_t)specific_camera);
    } else if (test_all) {
        for (uint32_t i = 0; i < ndev; i++) to_test.push_back(i);
    } else {
        std::map<std::string, uint32_t> seen;
        for (const auto& cam : cameras) {
            bool is_arducam = (cam.name.find("Arducam") != std::string::npos ||
                               cam.name.find("arducam") != std::string::npos ||
                               cam.name.find("OV9281") != std::string::npos);
            if (is_arducam) {
                printf(">>> Arducam/OV9281 at index %u\n", cam.index);
                to_test.push_back(cam.index);
            } else if (seen.find(cam.name) == seen.end()) {
                seen[cam.name] = cam.index;
                printf(">>> \"%s\" (index %u) — representative\n",
                       cam.name.c_str(), cam.index);
                to_test.push_back(cam.index);
            } else {
                printf(">>> Skipping \"%s\" (index %u) — duplicate of index %u\n",
                       cam.name.c_str(), cam.index, seen[cam.name]);
            }
        }
    }

    printf("\n");
    print_sep();
    printf("  Testing %zu camera(s)\n", to_test.size());
    print_sep();

    // ── Phase 3: Test FPS ────────────────────────────────────────────────
    for (uint32_t ci : to_test) {
        const auto& cam = cameras[ci];
        printf("\n=== Camera %u: \"%s\" ===\n", ci, cam.name.c_str());

        if (cam.formats.empty()) {
            printf("  No formats — skip\n\n");
            continue;
        }

        // Prefer MJPG, fall back to any format
        std::vector<CapFormat> mjpg;
        for (const auto& f : cam.formats)
            if (f.fourcc == 0x47504A4D) mjpg.push_back(f);
        auto& fmts = mjpg.empty() ? cam.formats : mjpg;

        // Group by resolution, collect unique FPS per resolution
        std::map<std::pair<uint32_t,uint32_t>, std::vector<CapFormat>> by_res;
        for (const auto& f : fmts)
            by_res[{f.width, f.height}].push_back(f);

        // Find resolutions to test:
        //   a) the one with the most unique FPS values
        //   b) the highest-resolution format (area = w*h)
        struct {
            std::pair<uint32_t,uint32_t> res;
            std::vector<CapFormat> unique_fps;
            std::string label;
        } test_resolutions[2];
        int n_test_res = 0;

        // a) Most FPS options
        {
            std::pair<uint32_t,uint32_t> best_res = {0,0};
            size_t best_fps_count = 0;
            for (const auto& kv : by_res) {
                std::set<uint32_t> ufps;
                for (const auto& f : kv.second) ufps.insert(f.fps);
                if (ufps.size() > best_fps_count) {
                    best_fps_count = ufps.size();
                    best_res = kv.first;
                }
            }
            if (best_res.first != 0) {
                std::set<uint32_t> seen;
                for (const auto& f : by_res[best_res])
                    if (seen.insert(f.fps).second)
                        test_resolutions[n_test_res].unique_fps.push_back(f);
                test_resolutions[n_test_res].res = best_res;
                test_resolutions[n_test_res].label = "most FPS options";
                n_test_res++;
            }
        }

        // b) Highest resolution (by area)
        {
            std::pair<uint32_t,uint32_t> hires = {0,0};
            uint64_t max_area = 0;
            for (const auto& kv : by_res) {
                uint64_t area = (uint64_t)kv.first.first * kv.first.second;
                if (area > max_area) { max_area = area; hires = kv.first; }
            }
            if (hires.first != 0) {
                bool same = (n_test_res > 0 && test_resolutions[0].res == hires);
                if (!same) {
                    std::set<uint32_t> seen;
                    for (const auto& f : by_res[hires])
                        if (seen.insert(f.fps).second)
                            test_resolutions[n_test_res].unique_fps.push_back(f);
                    test_resolutions[n_test_res].res = hires;
                    test_resolutions[n_test_res].label = "highest resolution";
                    n_test_res++;
                }
            }
        }

        if (n_test_res == 0) {
            printf("  No usable formats — skip\n\n");
            continue;
        }

        for (int ti = 0; ti < n_test_res; ti++) {
            auto& tr = test_resolutions[ti];
            printf("  [%s] %ux%u  (%zu unique FPS:",
                   tr.label.c_str(), tr.res.first, tr.res.second, tr.unique_fps.size());
            for (const auto& f : tr.unique_fps) printf(" %u", f.fps);
            printf(")\n");

            // ── Test A: open at lowest FPS, verify resolution ──
            {
                const auto& fmt = tr.unique_fps[0];
                printf("    [A] Open %ux%u @ %u fps ...\n", fmt.width, fmt.height, fmt.fps);

                int32_t stream = Cap_openStream(ctx, ci, fmt.id);
                if (stream < 0) {
                    printf("        FAIL: could not open stream (rc=%d)\n", stream);
                    continue;
                }

                // Verify resolution
                uint32_t actual_w = 0, actual_h = 0;
                if (Cap_getStreamResolution(ctx, stream, &actual_w, &actual_h) == CAPRESULT_OK) {
                    bool match = (actual_w == fmt.width && actual_h == fmt.height);
                    printf("        Resolution: %ux%u  %s\n", actual_w, actual_h,
                           match ? "MATCH" : "MISMATCH");
                }

                Sleep(500);
                double fps1 = measure_fps(ctx, stream);
                printf("        FPS measured: %.1f (expected ~%u)\n", fps1, fmt.fps);

                // ── Test B: open at highest FPS (different format) ──
                if (tr.unique_fps.size() >= 2) {
                    const auto& alt = tr.unique_fps.back();
                    Cap_closeStream(ctx, stream);

                    printf("    [B] Open %ux%u @ %u fps ...\n", alt.width, alt.height, alt.fps);
                    stream = Cap_openStream(ctx, ci, alt.id);
                    if (stream < 0) {
                        printf("        FAIL: could not open stream (rc=%d)\n", stream);
                        continue;
                    }

                    Sleep(500);
                    double fps2 = measure_fps(ctx, stream);
                    printf("        FPS measured: %.1f (expected ~%u)  %s\n",
                           fps2, alt.fps,
                           (fabs(fps2 - alt.fps) < 10.0) ? "OK" : "DELTA");
                }

                // ── Test C: runtime FPS change ──
                if (tr.unique_fps.size() >= 2) {
                    // Try a different FPS from what we opened at
                    uint32_t target_fps = 0;
                    for (const auto& f : tr.unique_fps) {
                        if (f.fps != tr.unique_fps.back().fps) {
                            target_fps = f.fps;
                            break;
                        }
                    }
                    if (target_fps > 0) {
                        printf("    [C] Cap_setFrameRate(%u) while stream is open ...\n", target_fps);
                        CapResult r = Cap_setFrameRate(ctx, stream, target_fps);
                        if (r == CAPRESULT_OK) {
                            Sleep(500);
                            double fps3 = measure_fps(ctx, stream);
                            printf("        FPS measured: %.1f (expected ~%u)  %s\n",
                                   fps3, target_fps,
                                   (fabs(fps3 - target_fps) < 10.0) ? "OK" : "DELTA");
                        } else {
                            printf("        Cap_setFrameRate returned %d\n", r);
                        }
                    }
                }

                Cap_closeStream(ctx, stream);
                printf("\n");
            }
        }

    }

    printf("\n");
    print_sep();
    printf("  Test complete.\n");
    print_sep();

    Cap_releaseContext(ctx);
    return 0;
}
