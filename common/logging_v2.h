/*
    Zero-cost logging macros for OpenPnP Capture library.
    Built on spdlog with compile-time level stripping.

    Include this header in files that need structured, zero-cost logging.
    Use logging.h for the legacy printf-style LOG() function.

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

#ifndef logging_v2_h
#define logging_v2_h

#ifdef __cplusplus

#include <stdint.h>
#include <memory>

// forward-declare the callback type instead of including logging.h, avoiding
// macro conflicts (logging.h defines LOG_DEBUG=7 / LOG_INFO=6 as constants;
// we redefine them as function-style macros below)
typedef void (*customLogFunc)(uint32_t logLevel, const char *logString);

// ---------------------------------------------------------------------------
// compile-time log level — set by CMake (target_compile_definitions).
// Fallback: debug builds compile everything; release builds strip TRACE+DEBUG.
// ---------------------------------------------------------------------------
#ifndef SPDLOG_ACTIVE_LEVEL
#  ifdef NDEBUG
#    define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#  else
#    define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#  endif
#endif

#include <spdlog/spdlog.h>

// ---------------------------------------------------------------------------
// convenience macros — zero-cost when stripped by SPDLOG_ACTIVE_LEVEL
//
// Usage:
//   LOG_TRACE("device={}  frame={}  dropped={}", dev_id, frame, dropped);
//   LOG_DEBUG("resizing buffer {} -> {}", old_size, new_size);
//   LOG_INFO("stream opened: {}x{} {}", width, height, format);
//   LOG_WARN("exposure={} may not have stuck on macOS", exposure_val);
//   LOG_ERROR("VIDIOC_STREAMON failed, errno={}", errno);
//
// When a level is compiled out (e.g. TRACE in a release build), the entire
// call disappears — no argument evaluation, no branch, no codegen.
// ---------------------------------------------------------------------------
#define LOG_TRACE(...)    SPDLOG_TRACE(__VA_ARGS__)

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4005) // intentional macro redefinition (see below)
#endif
#define LOG_DEBUG(...)    SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...)     SPDLOG_INFO(__VA_ARGS__)
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define LOG_WARN(...)     SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...)    SPDLOG_ERROR(__VA_ARGS__)

// ---------------------------------------------------------------------------
// logger instance access
// ---------------------------------------------------------------------------
namespace openpnp {
std::shared_ptr<spdlog::logger> getLogger();

// bridge functions — called from the legacy logging.cpp
void setLogCallback(customLogFunc func);
void setLogLevel(uint32_t level);
uint32_t getLogLevel();
}

// for code that needs the raw spdlog logger (e.g. creating child loggers)
#define LOG_LOGGER() openpnp::getLogger()

#endif // __cplusplus
#endif // logging_v2_h
