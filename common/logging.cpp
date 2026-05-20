/*
    spdlog-based logging for OpenPnP Capture library.

    Owns the spdlog logger instance and provides:
    - getLogger()       — singleton logger with stderr + callback sinks
    - setLogCallback()  — update the custom callback sink
    - setLogLevel()     — runtime level control, mapping legacy levels to spdlog
    - getLogLevel()     — reverse mapping from spdlog to legacy level constants

    Also implements the global public API wrappers declared in logging.h.

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

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/pattern_formatter.h>
#include <cctype>
#include <mutex>

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
// custom flag formatter — uppercase level names (spdlog's %l is lowercase)
// ---------------------------------------------------------------------------
class flag_upper_level : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg& msg,
                const std::tm&,
                spdlog::memory_buf_t& dest) override
    {
        auto sv = spdlog::level::to_string_view(msg.level);
        auto start = dest.size();
        dest.append(sv.data(), sv.data() + sv.size());
        for (auto i = start; i != dest.size(); ++i) {
            dest[i] = static_cast<char>(
                std::toupper(static_cast<unsigned char>(dest[i])));
        }
    }

    std::unique_ptr<custom_flag_formatter> clone() const override {
        return std::unique_ptr<custom_flag_formatter>(new flag_upper_level());
    }
};

// ---------------------------------------------------------------------------
// logger singleton
// ---------------------------------------------------------------------------
std::shared_ptr<spdlog::logger> openpnp::getLogger() {
    static std::shared_ptr<spdlog::logger> logger = []() {
        auto stderr_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        auto cb_sink     = std::make_shared<callback_sink_mt>();

        getCallbackSink() = cb_sink;

        auto lgr = std::make_shared<spdlog::logger>(
            "openpnp-capture",
            spdlog::sinks_init_list{stderr_sink, cb_sink});

        spdlog::register_logger(lgr);
        lgr->set_level(spdlog::level::debug);
        lgr->flush_on(spdlog::level::err);

        //  format matched to skellylogs: message-first with pipe separators
        // %* = custom uppercase level flag, %n = package name, %g:%# = file:line
        auto formatter = std::make_unique<spdlog::pattern_formatter>();
        formatter->add_flag<flag_upper_level>('*');
        formatter->set_pattern(
            ">>  %v |  %^%-8*%$ |  %n |  %g:%# |  %Y-%m-%dT%H:%M:%S.%e |  PID:%P |  TID:%t");
        lgr->set_formatter(std::move(formatter));

        return lgr;
    }();
    return logger;
}

// ---------------------------------------------------------------------------
// openpnp namespace — bridge functions called by global public API wrappers
// ---------------------------------------------------------------------------
void openpnp::setLogCallback(customLogFunc func) {
    getLogger(); // ensure initialized
    auto& sink = getCallbackSink();
    if (sink) {
        sink->set_callback(func);
    }
}

void openpnp::setLogLevel(uint32_t level) {
    auto logger = getLogger();
    spdlog::level::level_enum lvl;
    switch (level) {
        case LOG_EMERG:  case LOG_ALERT:  case LOG_CRIT_VAL:
            lvl = spdlog::level::critical;  break;
        case LOG_ERR:
            lvl = spdlog::level::err;       break;
        case LOG_WARNING:
            lvl = spdlog::level::warn;      break;
        case LOG_NOTICE: case LOG_INFO_VAL:
            lvl = spdlog::level::info;      break;
        case LOG_DEBUG_VAL:
            lvl = spdlog::level::debug;     break;
        case LOG_VERBOSE:
        default:
            lvl = spdlog::level::trace;     break;
    }
    logger->set_level(lvl);
}

uint32_t openpnp::getLogLevel() {
    auto logger = getLogger();
    switch (logger->level()) {
        case spdlog::level::critical:  return LOG_CRIT_VAL;
        case spdlog::level::err:       return LOG_ERR;
        case spdlog::level::warn:      return LOG_WARNING;
        case spdlog::level::info:      return LOG_INFO_VAL;
        case spdlog::level::debug:     return LOG_DEBUG_VAL;
        case spdlog::level::trace:
        default:                       return LOG_VERBOSE;
    }
}

// ---------------------------------------------------------------------------
// global public API wrappers — called from libmain.cpp (Cap_setLogLevel etc.)
// ---------------------------------------------------------------------------

static customLogFunc gs_logFunc = nullptr;

void installCustomLogFunction(customLogFunc logfunc) {
    gs_logFunc = logfunc;
    openpnp::setLogCallback(logfunc);
}

void setLogLevel(uint32_t logLevel) {
    openpnp::setLogLevel(logLevel);
}

uint32_t getLogLevel() {
    return openpnp::getLogLevel();
}
