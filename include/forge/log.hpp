#pragma once
// Logging built on spdlog.
//
// All loggers share one distribution sink, so sinks (console, rotating file,
// syslog, interactive console) can be added or swapped at runtime and every
// named logger follows automatically.

#include <spdlog/spdlog.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace forge {

using Logger = std::shared_ptr<spdlog::logger>;

namespace log {

struct Options {
    std::string name = "app";  ///< name of the default logger
    std::string level = "info";  ///< trace|debug|info|warn|error|critical|off
    std::string pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v";
    bool console = true;         ///< log to stderr (colored when it is a tty)
    std::string file;            ///< rotating log file, empty = disabled
    std::size_t max_size_mb = 10;
    std::size_t max_files = 3;
    bool syslog = false;         ///< also log to syslog (LOG_DAEMON)
    std::string flush_level = "warn";
    int flush_interval_s = 2;    ///< periodic flush, 0 = disabled
};

/// (Re)initialise logging. Safe to call more than once.
void init(const Options& options);

/// Default logger (same as spdlog::default_logger()).
Logger get();

/// Named logger sharing the global sinks; created on first use.
Logger get(const std::string& name);

/// Parse a level name. Throws std::invalid_argument for unknown names.
spdlog::level::level_enum parse_level(std::string_view level);

/// Set the level of every logger (current and future).
void set_level(spdlog::level::level_enum level);
void set_level(std::string_view level);
spdlog::level::level_enum level();

/// Add / remove an extra sink (shared by all loggers).
void add_sink(const spdlog::sink_ptr& sink);
void remove_sink(const spdlog::sink_ptr& sink);

/// Replace the console sink (used by forge::Console to keep logs from
/// corrupting the prompt). Returns the previous console sink (may be null).
spdlog::sink_ptr set_console_sink(spdlog::sink_ptr sink);

/// Pattern applied to sinks created by init() and set_console_sink().
std::string pattern();

void flush();
void shutdown();

using spdlog::critical;
using spdlog::debug;
using spdlog::error;
using spdlog::info;
using spdlog::trace;
using spdlog::warn;

}  // namespace log
}  // namespace forge

// Macros include file/line information and compile out below SPDLOG_ACTIVE_LEVEL.
#define FORGE_TRACE(...)    SPDLOG_TRACE(__VA_ARGS__)
#define FORGE_DEBUG(...)    SPDLOG_DEBUG(__VA_ARGS__)
#define FORGE_INFO(...)     SPDLOG_INFO(__VA_ARGS__)
#define FORGE_WARN(...)     SPDLOG_WARN(__VA_ARGS__)
#define FORGE_ERROR(...)    SPDLOG_ERROR(__VA_ARGS__)
#define FORGE_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)
