/*
    Performance benchmark: raw JPEG capture vs JPEG→RGB decode.

    Opens a single raw MJPEG stream and times two operations per frame:
      1. Cap_captureFrameRaw  — copy compressed JPEG bytes to user buffer
      2. Cap_decodeFrame      — decompress those bytes to 24-bit RGB

    The difference isolates the libjpeg-turbo decompression cost.
*/

#include <stdio.h>
#include <stdint.h>
#include <windows.h>
#include <chrono>
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>

#include "openpnp-capture.h"

static const uint32_t BENCH_DURATION_SEC = 10;
static const uint32_t WARMUP_FRAMES     = 30;

static std::string FourCCToString(uint32_t fourcc)
{
    std::string v;
    for (uint32_t i = 0; i < 4; i++) {
        v += static_cast<char>(fourcc & 0xFF);
        fourcc >>= 8;
    }
    return v;
}

static double median(std::vector<double> &v)
{
    if (v.empty()) return 0.0;
    size_t n = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + n, v.end());
    return v[n];
}

static double avg(const std::vector<double> &v)
{
    if (v.empty()) return 0.0;
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}

static double pct(std::vector<double> v, double pct)
{
    if (v.empty()) return 0.0;
    size_t idx = static_cast<size_t>(v.size() * pct / 100.0);
    if (idx >= v.size()) idx = v.size() - 1;
    std::nth_element(v.begin(), v.begin() + idx, v.end());
    return v[idx];
}

int main(int argc, char *argv[])
{
    uint32_t deviceID   = 0;
    int32_t  mjpegFmtID = -1;

    Cap_setLogLevel(3);

    printf("========================================\n");
    printf(" JPEG Decode Overhead Benchmark\n");
    printf(" %s\n", Cap_getLibraryVersion());
    printf("========================================\n\n");

    if (argc >= 2) deviceID = atoi(argv[1]);

    CapContext ctx = Cap_createContext();
    if (!ctx) {
        fprintf(stderr, "ERROR: Failed to create context\n");
        return 1;
    }

    uint32_t devCount = Cap_getDeviceCount(ctx);
    printf("Found %u device(s)\n", devCount);
    for (uint32_t i = 0; i < devCount; i++) {
        printf("  %u: %s\n", i, Cap_getDeviceName(ctx, i));
        int32_t nFmts = Cap_getNumFormats(ctx, i);
        for (int32_t j = 0; j < nFmts; j++) {
            CapFormatInfo info;
            Cap_getFormatInfo(ctx, i, j, &info);
            std::string fc = FourCCToString(info.fourcc);
            printf("      %d: %ux%u @ %u fps  %s",
                   j, info.width, info.height, info.fps, fc.c_str());
            if (info.fourcc == 0x47504A4D && i == deviceID) {
                printf("  <-- MJPEG");
                if (mjpegFmtID == -1) mjpegFmtID = j;
            }
            printf("\n");
        }
    }

    if (mjpegFmtID == -1) {
        fprintf(stderr, "\nERROR: No MJPEG format on device %u\n", deviceID);
        Cap_releaseContext(ctx);
        return 1;
    }

    CapFormatInfo finfo;
    Cap_getFormatInfo(ctx, deviceID, mjpegFmtID, &finfo);
    uint32_t rgbBufSize = finfo.width * finfo.height * 3;

    printf("\nDevice: %s\n", Cap_getDeviceName(ctx, deviceID));
    printf("Format: %ux%u MJPEG  (RGB buffer: %u bytes)\n\n",
           finfo.width, finfo.height, rgbBufSize);

    CapStream streamID = Cap_openStreamRaw(ctx, deviceID, mjpegFmtID);
    if (streamID < 0) {
        fprintf(stderr, "ERROR: Cap_openStreamRaw failed\n");
        Cap_releaseContext(ctx);
        return 1;
    }

    std::vector<uint8_t> jpegBuf(rgbBufSize / 2);
    std::vector<uint8_t> rgbBuf(rgbBufSize);

    // warmup
    printf("Warmup (%u frames)...\n", WARMUP_FRAMES);
    for (uint32_t n = 0; n < WARMUP_FRAMES; ) {
        if (!Cap_hasNewFrame(ctx, streamID)) { Sleep(1); continue; }
        uint32_t actual = 0;
        CapResult r = Cap_captureFrameRaw(ctx, streamID,
            jpegBuf.data(), (uint32_t)jpegBuf.size(), &actual);
        if (r == CAPRESULT_ERR && actual > jpegBuf.size()) {
            jpegBuf.resize(actual * 2);
            continue;
        }
        if (r != CAPRESULT_OK) continue;
        Cap_decodeFrame(ctx, streamID, rgbBuf.data(), rgbBufSize);
        n++;
    }
    printf("Warmup complete.\n\n");

    // benchmark
    std::vector<double> rawUs, decodeUs;
    rawUs.reserve(10000);
    decodeUs.reserve(10000);

    auto benchEnd = std::chrono::steady_clock::now()
                  + std::chrono::seconds(BENCH_DURATION_SEC);

    uint32_t frames = 0, skipped = 0;
    uint64_t totalJpegBytes = 0;

    printf("Benchmarking (%u s)...\n", BENCH_DURATION_SEC);

    while (std::chrono::steady_clock::now() < benchEnd) {
        if (!Cap_hasNewFrame(ctx, streamID)) { Sleep(1); continue; }

        // --- time: copy raw JPEG bytes ---
        auto t0 = std::chrono::steady_clock::now();
        uint32_t actual = 0;
        CapResult r = Cap_captureFrameRaw(ctx, streamID,
            jpegBuf.data(), (uint32_t)jpegBuf.size(), &actual);
        auto t1 = std::chrono::steady_clock::now();

        if (r == CAPRESULT_ERR && actual > jpegBuf.size()) {
            jpegBuf.resize(actual * 2);
            skipped++;
            continue;
        }
        if (r != CAPRESULT_OK) { skipped++; continue; }

        rawUs.push_back(
            std::chrono::duration<double, std::micro>(t1 - t0).count());

        // --- time: decode JPEG → RGB ---
        auto t2 = std::chrono::steady_clock::now();
        CapResult dr = Cap_decodeFrame(ctx, streamID,
            rgbBuf.data(), rgbBufSize);
        auto t3 = std::chrono::steady_clock::now();

        if (dr == CAPRESULT_OK) {
            decodeUs.push_back(
                std::chrono::duration<double, std::micro>(t3 - t2).count());
        }

        frames++;
        totalJpegBytes += actual;
    }

    // report
    printf("\n");
    printf("========================================\n");
    printf(" RESULTS  (%u frames, %u skipped)\n", frames, skipped);
    printf("========================================\n\n");

    if (rawUs.empty()) {
        printf("No frames captured.\n");
        Cap_closeStream(ctx, streamID);
        Cap_releaseContext(ctx);
        return 1;
    }

    double avgJpegKB = (totalJpegBytes / 1024.0) / frames;

    printf("  JPEG size :  avg %.1f KB  (%.1f:1 compression vs %u-byte RGB)\n\n",
           avgJpegKB, (rgbBufSize / 1024.0) / avgJpegKB, rgbBufSize);

    printf("  ── Cap_captureFrameRaw  (copy JPEG bytes) ──\n");
    printf("    mean   : %8.1f us  (%7.3f ms)\n", avg(rawUs), avg(rawUs) / 1000.0);
    printf("    median : %8.1f us\n", median(rawUs));
    printf("    p95    : %8.1f us\n", pct(rawUs, 95));
    printf("    p99    : %8.1f us\n", pct(rawUs, 99));

    if (!decodeUs.empty()) {
        printf("\n  ── Cap_decodeFrame  (JPEG → RGB) ──\n");
        printf("    mean   : %8.1f us  (%7.3f ms)\n",
               avg(decodeUs), avg(decodeUs) / 1000.0);
        printf("    median : %8.1f us\n", median(decodeUs));
        printf("    p95    : %8.1f us\n", pct(decodeUs, 95));
        printf("    p99    : %8.1f us\n", pct(decodeUs, 99));

        double totalUs = avg(rawUs) + avg(decodeUs);

        printf("\n  ── Summary ──\n");
        printf("    raw copy       : %7.1f us\n", avg(rawUs));
        printf("    decode         : %7.1f us\n", avg(decodeUs));
        printf("    total per frame: %7.1f us  (%.3f ms)\n",
               totalUs, totalUs / 1000.0);
        printf("\n    decode is %.1f%% of per-frame cost\n",
               avg(decodeUs) / totalUs * 100.0);
        printf("    raw-only ceiling : %.1f fps\n", 1e6 / avg(rawUs));
        printf("    raw+decode ceiling: %.1f fps\n", 1e6 / totalUs);
    }

    printf("\n========================================\n");

    Cap_closeStream(ctx, streamID);
    Cap_releaseContext(ctx);
    return 0;
}
