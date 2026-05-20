/*
    spdlog-based logging for OpenPnP Capture library.

    Owns the spdlog logger instance and provides:
    - getLogger()       — singleton logger with stderr + callback sinks
    - setLogCallback()  — update the custom callback sink (called from logging.cpp)
    - setLogLevel()     — runtime level control, mapping legacy levels to spdlog
    - getLogLevel()     — reverse mapping from spdlog to legacy level constants

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

#include "logging.h"
#include "logging_v2.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/base_sink.h>
#include <mutex>

namespace openpnp {

// ---------------------------------------------------------------------------
// custom sink — forwards formatted messages to the user's CapCustomLogFunc
// ---------------------------------------------------------------------------
template<typename Mutex>
class callback_sink final : public spdlog::sinks::base_sink<Mutex> {
public:
    void set_callback(customLogFunc func) {
        std::lock_guard<Mutex> lock(spdlog::sinks::base_sink<Mutex>::mutex_);
        m_func = func;
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        if (m_func) {
            spdlog::memory_buf_t formatted;
            spdlog::sinks::base_sink<Mutex>::formatter_->format(msg, formatted);
            m_func(static_cast<uint32_t>(msg.level), fmt::to_string(formatted).c_str());
        }
    }

    void flush_() override {}

private:
    customLogFunc m_func = nullptr;
};

using callback_sink_mt = callback_sink<std::mutex>;

// ---------------------------------------------------------------------------
// sinks access — function-local statics for thread-safe lazy init (C++11)
// ---------------------------------------------------------------------------
namespace {
    std::shared_ptr<callback_sink_mt>& getCallbackSink() {
        static std::shared_ptr<callback_sink_mt> sink;
        return sink;
    }
}

// ---------------------------------------------------------------------------
// logger singleton
// ---------------------------------------------------------------------------
std::shared_ptr<spdlog::logger> getLogger() {
    static std::shared_ptr<spdlog::logger> logger = []() {
        auto stderr_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        auto cb_sink     = std::make_shared<callback_sink_mt>();

        getCallbackSink() = cb_sink;

        auto lgr = std::make_shared<spdlog::logger>(
            "openpnp-capture",
            spdlog::sinks_init_list{stderr_sink, cb_sink});

        spdlog::register_logger(lgr);
        lgr->set_level(spdlog::level::trace);
        lgr->flush_on(spdlog::level::err);

        // compact pattern: [LEVEL] YYYY-MM-DD HH:MM:SS.mmm [thread] msg
        lgr->set_pattern("%^[%L] %Y-%m-%d %H:%M:%S.%e [%t] %v%$");

        return lgr;
    }();
    return logger;
}

// ---------------------------------------------------------------------------
// legacy API bridge — called from logging.cpp
// ---------------------------------------------------------------------------
void setLogCallback(customLogFunc func) {
    getLogger(); // ensure initialized
    auto& sink = getCallbackSink();
    if (sink) {
        sink->set_callback(func);
    }
}

void setLogLevel(uint32_t level) {
    auto logger = getLogger();
    spdlog::level::level_enum lvl;
    switch (level) {
        case LOG_EMERG:  case LOG_ALERT:  case LOG_CRIT:
            lvl = spdlog::level::critical;  break;
        case LOG_ERR:
            lvl = spdlog::level::err;       break;
        case LOG_WARNING:
            lvl = spdlog::level::warn;      break;
        case 5: case 6:
            lvl = spdlog::level::info;      break;
        case 7:
            lvl = spdlog::level::debug;     break;
        case LOG_VERBOSE:
        default:
            lvl = spdlog::level::trace;     break;
    }
    logger->set_level(lvl);
}

uint32_t getLogLevel() {
    auto logger = getLogger();
    switch (logger->level()) {
        case spdlog::level::critical:  return LOG_CRIT;
        case spdlog::level::err:       return LOG_ERR;
        case spdlog::level::warn:      return LOG_WARNING;
        case spdlog::level::info:      return 6;
        case spdlog::level::debug:     return 7;
        case spdlog::level::trace:
        default:                       return LOG_VERBOSE;
    }
}

} // namespace openpnp
