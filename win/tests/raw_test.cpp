/*
    Raw MJPEG test program
    Tests the new Cap_openStreamRaw / Cap_captureFrameRaw / Cap_decodeFrame API
*/

#include <stdio.h>
#include <stdint.h>
#include <windows.h>  // for Sleep
#include <chrono>
#include <vector>
#include <string>

#include "openpnp-capture.h"

void myCustomLogFunction(uint32_t level, const char *string)
{
    printf("== %s", string);
}

std::string FourCCToString(uint32_t fourcc)
{
    std::string v;
    for (uint32_t i = 0; i < 4; i++) {
        v += static_cast<char>(fourcc & 0xFF);
        fourcc >>= 8;
    }
    return v;
}

bool isValidJPEG(const uint8_t *data, size_t length)
{
    if (length < 4) return false;
    // JPEG files start with SOI marker: 0xFF 0xD8
    return (data[0] == 0xFF && data[1] == 0xD8);
}

bool writeJPEG(const char *fname, const uint8_t *data, size_t length)
{
    FILE *f = fopen(fname, "wb");
    if (!f) {
        fprintf(stderr, "Cannot open %s for writing\n", fname);
        return false;
    }
    fwrite(data, 1, length, f);
    fclose(f);
    return true;
}

int main(int argc, char *argv[])
{
    uint32_t deviceID = 0;
    int32_t mjpegFormatID = -1;
    bool writeFrames = false;

    Cap_installCustomLogFunction(myCustomLogFunction);
    Cap_setLogLevel(7);  // LOG_DEBUG

    printf("========================================\n");
    printf(" OpenPNP Raw MJPEG Capture Test\n");
    printf(" %s\n", Cap_getLibraryVersion());
    printf("========================================\n\n");

    if (argc >= 2) {
        deviceID = atoi(argv[1]);
    }
    if (argc >= 3 && strcmp(argv[2], "--write") == 0) {
        writeFrames = true;
    }

    CapContext ctx = Cap_createContext();
    if (!ctx) {
        fprintf(stderr, "ERROR: Failed to create context\n");
        return 1;
    }

    // List devices
    uint32_t deviceCount = Cap_getDeviceCount(ctx);
    printf("Found %u device(s)\n\n", deviceCount);
    for (uint32_t i = 0; i < deviceCount; i++) {
        printf("  Device %u: %s\n", i, Cap_getDeviceName(ctx, i));
        printf("  Unique: %s\n", Cap_getDeviceUniqueID(ctx, i));

        int32_t nFormats = Cap_getNumFormats(ctx, i);
        printf("  Formats: %d\n", nFormats);
        for (int32_t j = 0; j < nFormats; j++) {
            CapFormatInfo info;
            Cap_getFormatInfo(ctx, i, j, &info);
            std::string fourcc = FourCCToString(info.fourcc);
            printf("    Format %d: %ux%u @ %u fps, %u bpp, FOURCC=%s",
                   j, info.width, info.height, info.fps, info.bpp, fourcc.c_str());

            // Find MJPEG format
            if (info.fourcc == 0x47504A4D && i == deviceID) {
                printf(" <-- MJPEG (will use this)");
                if (mjpegFormatID == -1) mjpegFormatID = j;
            }
            printf("\n");
        }
    }

    if (mjpegFormatID == -1) {
        fprintf(stderr, "\nERROR: No MJPEG format found for device %u\n", deviceID);
        fprintf(stderr, "This test requires an MJPEG-capable camera.\n");
        Cap_releaseContext(ctx);
        return 1;
    }

    // Get format info
    CapFormatInfo finfo;
    Cap_getFormatInfo(ctx, deviceID, mjpegFormatID, &finfo);
    printf("\n--- Opening RAW stream ---\n");
    printf("Device: %s\n", Cap_getDeviceName(ctx, deviceID));
    printf("Format: %ux%u @ %u fps, FOURCC=MJPG\n",
           finfo.width, finfo.height, finfo.fps);

    // TEST 1: Open raw stream
    CapStream streamID = Cap_openStreamRaw(ctx, deviceID, mjpegFormatID);
    printf("Raw stream ID = %d\n\n", streamID);

    if (streamID < 0) {
        fprintf(stderr, "ERROR: Cap_openStreamRaw failed\n");
        Cap_releaseContext(ctx);
        return 1;
    }

    if (Cap_isOpenStream(ctx, streamID) == 1) {
        printf("PASS: Raw stream is open\n\n");
    } else {
        fprintf(stderr, "FAIL: Raw stream reports as closed\n");
        Cap_closeStream(ctx, streamID);
        Cap_releaseContext(ctx);
        return 1;
    }

    // TEST 2: Wait for frames and validate
    printf("--- Waiting for frames (5 seconds) ---\n");

    std::vector<uint8_t> jpegBuffer;
    jpegBuffer.resize(finfo.width * finfo.height * 3 / 4);  // initial estimate

    std::vector<uint8_t> rgbBuffer;
    rgbBuffer.resize(finfo.width * finfo.height * 3);

    uint32_t framesReceived = 0;
    uint32_t minJPEGSize = 0xFFFFFFFF;
    uint32_t maxJPEGSize = 0;
    uint32_t totalJPEGBytes = 0;
    uint32_t bufferGrows = 0;
    uint32_t validJPEGCount = 0;
    uint32_t decodeSuccessCount = 0;
    uint32_t lastFrameCount = 0;

    auto startTime = std::chrono::steady_clock::now();
    auto endTime = startTime + std::chrono::seconds(5);

    while (std::chrono::steady_clock::now() < endTime) {
        if (Cap_hasNewFrame(ctx, streamID)) {
            uint32_t actualBytes = 0;
            CapResult retry = CAPRESULT_ERR;
            int retryCount = 0;

            // Capture with buffer growth retry
            while (retryCount < 3) {
                retry = Cap_captureFrameRaw(ctx, streamID,
                    jpegBuffer.data(), (uint32_t)jpegBuffer.size(), &actualBytes);
                if (retry == CAPRESULT_OK) break;
                // Buffer too small — grow and retry
                jpegBuffer.resize(actualBytes * 2);
                printf("  (Retry %d: grew buffer to %zu bytes)\n",
                       retryCount + 1, jpegBuffer.size());
                bufferGrows++;
                retryCount++;
            }

            if (retry != CAPRESULT_OK) {
                fprintf(stderr, "FAIL: Cap_captureFrameRaw failed after retries\n");
                continue;
            }

            framesReceived++;
            if (actualBytes < minJPEGSize) minJPEGSize = actualBytes;
            if (actualBytes > maxJPEGSize) maxJPEGSize = actualBytes;
            totalJPEGBytes += actualBytes;

            // TEST 3a: Validate JPEG header
            if (isValidJPEG(jpegBuffer.data(), actualBytes)) {
                validJPEGCount++;
            } else {
                printf("  WARNING: Frame %u does not start with JPEG SOI (0xFFD8)\n",
                       framesReceived);
                printf("    First bytes: %02X %02X %02X %02X\n",
                       jpegBuffer[0], jpegBuffer[1], jpegBuffer[2], jpegBuffer[3]);
            }

            // TEST 3b: Decode on demand (every 30th frame to keep things fast)
            if (framesReceived % 30 == 0) {
                if (Cap_decodeFrame(ctx, streamID, rgbBuffer.data(),
                                    (uint32_t)rgbBuffer.size()) == CAPRESULT_OK) {
                    decodeSuccessCount++;
                } else {
                    printf("  WARNING: Cap_decodeFrame failed on frame %u\n",
                           framesReceived);
                }
            }

            // Write first 5 frames as JPEG files if --write enabled
            if (writeFrames && framesReceived <= 5) {
                char fname[64];
                sprintf(fname, "raw_frame_%u.jpg", framesReceived);
                writeJPEG(fname, jpegBuffer.data(), actualBytes);
                printf("  Wrote %s (%u bytes)\n", fname, actualBytes);
            }

            // Progress every 100 frames
            uint32_t frameCount = Cap_getStreamFrameCount(ctx, streamID);
            if (frameCount - lastFrameCount >= 100) {
                lastFrameCount = frameCount;
                printf("  Frames captured: %u (avg size: %u bytes)\n",
                       frameCount, totalJPEGBytes / framesReceived);
            }
        }
        Sleep(1);
    }

    uint32_t totalFrames = Cap_getStreamFrameCount(ctx, streamID);

    // TEST 4: Check frame size query
    uint32_t queriedSize = 0;
    CapResult sizeRet = Cap_getFrameSize(ctx, streamID, &queriedSize);
    printf("\n--- Results ---\n");
    printf("Total frames captured by stream: %u\n", totalFrames);
    printf("Frames consumed by our loop:     %u\n", framesReceived);
    printf("Min JPEG size:  %u bytes\n", minJPEGSize);
    printf("Max JPEG size:  %u bytes\n", maxJPEGSize);
    printf("Avg JPEG size:  %u bytes\n",
           framesReceived > 0 ? totalJPEGBytes / framesReceived : 0);
    printf("Buffer growths: %u\n", bufferGrows);
    printf("Valid JPEGs:    %u / %u\n", validJPEGCount, framesReceived);
    printf("Decode tests:   %u / %u successful\n",
           decodeSuccessCount, (framesReceived + 29) / 30);
    printf("Queried size:   %s (%u bytes)\n",
           sizeRet == CAPRESULT_OK ? "OK" : "FAIL", queriedSize);

    printf("\n=== TEST SUMMARY ===\n");
    bool allPass = true;

    printf("%s Raw stream open\n",
           Cap_isOpenStream(ctx, streamID) ? "PASS:" : "FAIL:");
    allPass = allPass && Cap_isOpenStream(ctx, streamID);

    printf("%s Frames received (%u > 0)\n",
           framesReceived > 0 ? "PASS:" : "FAIL:", framesReceived);
    allPass = allPass && (framesReceived > 0);

    printf("%s JPEG validation (%u/%u)\n",
           validJPEGCount == framesReceived ? "PASS:" : "FAIL:",
           validJPEGCount, framesReceived);
    allPass = allPass && (validJPEGCount == framesReceived);

    printf("%s Buffer growths <= 5 (%u)\n",
           bufferGrows <= 5 ? "PASS:" : "WARN:", bufferGrows);

    printf("%s Decode-on-demand (%u successes)\n",
           decodeSuccessCount > 0 ? "PASS:" : "FAIL:", decodeSuccessCount);
    allPass = allPass && (decodeSuccessCount > 0);

    printf("%s Frame size query\n",
           sizeRet == CAPRESULT_OK ? "PASS:" : "FAIL:");
    allPass = allPass && (sizeRet == CAPRESULT_OK);

    Cap_closeStream(ctx, streamID);
    Cap_releaseContext(ctx);

    if (allPass) {
        printf("\n*** ALL TESTS PASSED ***\n");
        return 0;
    } else {
        printf("\n*** SOME TESTS FAILED ***\n");
        return 1;
    }
}
