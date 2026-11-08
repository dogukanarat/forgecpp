#pragma once
// Logging: a forgecpp facade over Quill.
//
// A log call formats its message on the calling thread (std::format, checked at
// compile time) and hands it to a queue; a background thread writes it to the
// terminal, the log file, syslog, the history (panel, RPC, telnet) and the
// listeners. The calling thread never waits for a terminal or a disk.
//
//   logger()->info("pump at {} rpm", rpm);        // inside an Application / Component
//   forge::log::warn("disk {:.0f} % full", used);  // default logger
//   FORGE_ERROR("cannot open {}", path);           // the same, as a macro
//
// Real-time threads: FORGE_RT_* in <forge/log_rt.hpp> format on the background
// thread instead (no allocation for numbers and strings, never blocks).
//
// Colours: level colours on a terminal (not when it is not one, not with
// NO_COLOR, not under journald: there each line carries its syslog priority).

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace forge::v1 {
class LogRecord;
}

namespace forge {
namespace log {

enum class Level : int { trace = 0, debug, info, warn, error, critical, off };

/// Parse a level name (trace|debug|info|warn|warning|error|err|critical|fatal|off|none).
/// Throws std::invalid_argument for unknown names.
Level parse_level(std::string_view level);
/// "trace", "debug", "info", "warning", "error", "critical", "off" (JSON, forge.v1).
std::string_view to_string(Level level);
/// Three letters, so log lines line up: "TRC", "DBG", "INF", "WRN", "ERR", "CRT", "OFF".
std::string_view short_name(Level level);

/// A format string checked at compile time (std::format_string), with the place it is written.
template <class... Args>
struct FormatString {
    template <class S>
        requires std::convertible_to<const S&, std::string_view>
    consteval FormatString(const S& text, std::source_location where = std::source_location::current())
        : format(text), where(where) {}
    std::format_string<Args...> format;
    std::source_location where;
};
template <class... Args>
using Format = FormatString<std::type_identity_t<Args>...>;

/// A named logger. Cheap to copy; every copy is the same logger.
class Logger {
public:
    struct Impl;

    /// The default logger.
    Logger();
    explicit Logger(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

    const std::string& name() const;
    Level level() const;
    void set_level(Level level) const;
    void set_level(std::string_view level) const { set_level(parse_level(level)); }
    bool should_log(Level level) const { return level >= this->level() && level != Level::off; }
    /// Records at or above `level` are written out (and the file flushed) without delay.
    void flush_on(Level level) const;
    /// Wait until everything logged so far is written.
    void flush() const;

    template <class... Args>
    void log(Level level, Format<Args...> fmt, Args&&... args) const {
        if (should_log(level)) write(level, std::format(fmt.format, std::forward<Args>(args)...), fmt.where);
    }
    template <class... Args>
    void trace(Format<Args...> fmt, Args&&... args) const {
        log<Args...>(Level::trace, fmt, std::forward<Args>(args)...);
    }
    template <class... Args>
    void debug(Format<Args...> fmt, Args&&... args) const {
        log<Args...>(Level::debug, fmt, std::forward<Args>(args)...);
    }
    template <class... Args>
    void info(Format<Args...> fmt, Args&&... args) const {
        log<Args...>(Level::info, fmt, std::forward<Args>(args)...);
    }
    template <class... Args>
    void warn(Format<Args...> fmt, Args&&... args) const {
        log<Args...>(Level::warn, fmt, std::forward<Args>(args)...);
    }
    template <class... Args>
    void error(Format<Args...> fmt, Args&&... args) const {
        log<Args...>(Level::error, fmt, std::forward<Args>(args)...);
    }
    template <class... Args>
    void critical(Format<Args...> fmt, Args&&... args) const {
        log<Args...>(Level::critical, fmt, std::forward<Args>(args)...);
    }

    /// A message that is already text (no formatting).
    void write(Level level, std::string_view message,
               const std::source_location& where = std::source_location::current()) const;

    /// Pointer syntax as well: logger()->info(...) and logger().info(...) are the same.
    const Logger* operator->() const { return this; }
    explicit operator bool() const { return impl_ != nullptr; }
    const std::shared_ptr<Impl>& impl() const { return impl_; }

private:
    std::shared_ptr<Impl> impl_;
};

struct Options {
    std::string name = "app";    ///< name of the default logger
    std::string level = "info";  ///< trace|debug|info|warn|error|critical|off
    /// Text lines: {time} {level} {logger} {message} {thread} {file} {line}
    /// ({level}: TRC DBG INF WRN ERR CRT).
    std::string pattern = "[{time}] [{level}] [{logger}] {message}";
    bool console = true;         ///< log to stderr
    /// Level colours on stderr: auto (a terminal, no NO_COLOR, not journald) | always | never.
    std::string color = "auto";
    std::string file;            ///< rotating log file, empty = disabled
    std::size_t max_size_mb = 10;
    std::size_t max_files = 3;
    bool syslog = false;         ///< also log to syslog (LOG_DAEMON)
    std::string flush_level = "warn";
    int flush_interval_s = 2;    ///< the file is flushed at least this often
    /// text: `pattern`; json: one JSON object per line (time, level, logger,
    /// message, thread, trace) for log collectors (stderr and the file).
    std::string format = "text";
    std::size_t history = 500;   ///< records kept for the panel, RPC and telnet
};

/// (Re)initialise logging. Safe to call more than once.
void init(const Options& options);

/// The default logger.
Logger get();
/// Named logger sharing the global outputs; created on first use.
Logger get(const std::string& name);

/// Set the level of every logger (current and future).
void set_level(Level level);
void set_level(std::string_view level);
Level level();
/// Level of one named logger (e.g. "radio"); others keep theirs.
void set_logger_level(const std::string& logger, std::string_view level);
/// Every logger and its level.
std::vector<std::pair<std::string, std::string>> logger_levels();

// --- history and live listeners (independent of the web UI) ---------------------
/// Newest last; limit 0 = everything kept.
std::vector<v1::LogRecord> history(std::size_t limit = 0);
using Listener = std::function<void(const v1::LogRecord&)>;
/// Called for every record that is logged (on the logging background thread).
/// A listener must not wait for a thread that is removing a listener.
int add_listener(Listener listener);
/// Returns once the listener is not running and will not be called again.
void remove_listener(int id);

/// Where terminal lines go instead of stderr while it is set (forge::Console
/// prints them above the line being typed). `plain` is the line as text,
/// `coloured` with ANSI level colours. Returns the previous writer (empty = stderr).
using TerminalWriter = std::function<void(std::string_view plain, std::string_view coloured)>;
TerminalWriter set_terminal_writer(TerminalWriter writer);

/// ANSI colour of a level (empty for off).
std::string_view level_color(Level level);

/// Wait until everything logged so far is written.
void flush();
/// Flush and stop the background thread (logging after this is dropped).
void shutdown();

template <class... Args>
void trace(Format<Args...> fmt, Args&&... args) {
    get().log<Args...>(Level::trace, fmt, std::forward<Args>(args)...);
}
template <class... Args>
void debug(Format<Args...> fmt, Args&&... args) {
    get().log<Args...>(Level::debug, fmt, std::forward<Args>(args)...);
}
template <class... Args>
void info(Format<Args...> fmt, Args&&... args) {
    get().log<Args...>(Level::info, fmt, std::forward<Args>(args)...);
}
template <class... Args>
void warn(Format<Args...> fmt, Args&&... args) {
    get().log<Args...>(Level::warn, fmt, std::forward<Args>(args)...);
}
template <class... Args>
void error(Format<Args...> fmt, Args&&... args) {
    get().log<Args...>(Level::error, fmt, std::forward<Args>(args)...);
}
template <class... Args>
void critical(Format<Args...> fmt, Args&&... args) {
    get().log<Args...>(Level::critical, fmt, std::forward<Args>(args)...);
}

}  // namespace log

using Logger = log::Logger;

}  // namespace forge

// The default logger, with the caller's file and line (shown with {file}:{line} in the pattern).
#define FORGE_TRACE(...)    ::forge::log::trace(__VA_ARGS__)
#define FORGE_DEBUG(...)    ::forge::log::debug(__VA_ARGS__)
#define FORGE_INFO(...)     ::forge::log::info(__VA_ARGS__)
#define FORGE_WARN(...)     ::forge::log::warn(__VA_ARGS__)
#define FORGE_ERROR(...)    ::forge::log::error(__VA_ARGS__)
#define FORGE_CRITICAL(...) ::forge::log::critical(__VA_ARGS__)
