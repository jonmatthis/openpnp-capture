/*
    Logging subsystem for OpenPnP Capture library.

    Backward-compatible printf-style API.  All calls delegate to the
    spdlog backend in logging_v2.cpp so that every message gets
    timestamps, thread IDs, colour output, and the custom-callback sink.

    New code should use logging_v2.h macros (zero-cost when compiled out)
    rather than this header.

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

#include <stdio.h>
#include <stdarg.h>
#include <spdlog/spdlog.h>
#include "logging.h"

// forward-declare openpnp bridge functions (logging_v2.cpp) — we cannot
// include logging_v2.h here because its LOG_INFO/LOG_DEBUG macros would
// collide with the legacy #define constants in logging.h
namespace openpnp {
std::shared_ptr<spdlog::logger> getLogger();
void setLogCallback(customLogFunc func);
void setLogLevel(uint32_t level);
uint32_t getLogLevel();
}

/* In their infinite "wisdom" Microsoft have declared snprintf is deprecated
   and we must therefore resort to a macro to fix something that shouldn't
   be a problem */
#ifdef _MSC_VER
#define snprintf _snprintf
#endif

static customLogFunc gs_logFunc = nullptr;

// ---------------------------------------------------------------------------
// legacy level constants → spdlog level enum
// ---------------------------------------------------------------------------
static spdlog::level::level_enum oldLevelToSpdlog(uint32_t logLevel)
{
    switch (logLevel) {
        case LOG_EMERG:  case LOG_ALERT:  case LOG_CRIT:
            return spdlog::level::critical;
        case LOG_ERR:     return spdlog::level::err;
        case LOG_WARNING: return spdlog::level::warn;
        case LOG_NOTICE:  case LOG_INFO:
            return spdlog::level::info;
        case LOG_DEBUG:   return spdlog::level::debug;
        case LOG_VERBOSE:
        default:          return spdlog::level::trace;
    }
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void installCustomLogFunction(customLogFunc logfunc)
{
    gs_logFunc = logfunc;
    openpnp::setLogCallback(logfunc);
}

void LOG(uint32_t logLevel, const char *format, ...)
{
    auto logger = openpnp::getLogger();
    auto spdLevel = oldLevelToSpdlog(logLevel);

    // cheap gate — skip the vsnprintf if this level isn't active
    if (!logger->should_log(spdLevel)) {
        return;
    }

    char msgbuf[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(msgbuf, sizeof(msgbuf), format, args);
    va_end(args);

    // route through spdlog for consistent formatting + callback sink
    logger->log(spdLevel, "{}", msgbuf);
}

void setLogLevel(uint32_t logLevel)
{
    openpnp::setLogLevel(logLevel);
}

uint32_t getLogLevel()
{
    return openpnp::getLogLevel();
}
