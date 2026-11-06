#include "forge/log.hpp"

#include <spdlog/sinks/ansicolor_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/syslog_sink.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <syslog.h>

namespace forge::log {
namespace {

struct State {
    std::mutex mutex;
    std::shared_ptr<spdlog::sinks::dist_sink_mt> dist;
    spdlog::sink_ptr console;
    std::string pattern = Options{}.pattern;
    spdlog::level::level_enum flush_level = spdlog::level::warn;
};

State& state() {
    static State s;
    return s;
}

// Must be called with state().mutex held.
std::shared_ptr<spdlog::sinks::dist_sink_mt> ensure_dist_locked() {
    auto& s = state();
    if (!s.dist) {
        s.dist = std::make_shared<spdlog::sinks::dist_sink_mt>();
        s.console = std::make_shared<spdlog::sinks::ansicolor_stderr_sink_mt>();
        s.console->set_pattern(s.pattern);
        s.dist->add_sink(s.console);
    }
    return s.dist;
}

Logger make_logger_locked(const std::string& name) {
    auto logger = std::make_shared<spdlog::logger>(name, ensure_dist_locked());
    logger->set_level(spdlog::get_level());
    logger->flush_on(state().flush_level);
    return logger;
}

}  // namespace

spdlog::level::level_enum parse_level(std::string_view level) {
    std::string l(level);
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return std::tolower(c); });
    if (l == "trace") return spdlog::level::trace;
    if (l == "debug") return spdlog::level::debug;
    if (l == "info") return spdlog::level::info;
    if (l == "warn" || l == "warning") return spdlog::level::warn;
    if (l == "error" || l == "err") return spdlog::level::err;
    if (l == "critical" || l == "fatal") return spdlog::level::critical;
    if (l == "off" || l == "none") return spdlog::level::off;
    throw std::invalid_argument("unknown log level '" + std::string(level) +
                                "' (expected trace|debug|info|warn|error|critical|off)");
}

void init(const Options& options) {
    const auto lvl = parse_level(options.level);
    const auto flush_lvl = parse_level(options.flush_level);

    auto dist = std::make_shared<spdlog::sinks::dist_sink_mt>();
    spdlog::sink_ptr console;
    if (options.console) {
        console = std::make_shared<spdlog::sinks::ansicolor_stderr_sink_mt>();
        console->set_pattern(options.pattern);
        dist->add_sink(console);
    }
    if (!options.file.empty()) {
        const std::filesystem::path path(options.file);
        if (path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
        }
        auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            options.file, std::max<std::size_t>(options.max_size_mb, 1) * 1024 * 1024,
            std::max<std::size_t>(options.max_files, 1));
        file->set_pattern(options.pattern);
        dist->add_sink(file);
    }
    if (options.syslog) {
        auto sys = std::make_shared<spdlog::sinks::syslog_sink_mt>(options.name, LOG_PID, LOG_DAEMON,
                                                                   true);
        sys->set_pattern("[%n] %v");
        dist->add_sink(sys);
    }

    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.dist = dist;
        s.console = console;
        s.pattern = options.pattern;
        s.flush_level = flush_lvl;
    }

    // Re-point already registered loggers to the new sink set.
    spdlog::apply_all([&](const Logger& l) {
        l->sinks().clear();
        l->sinks().push_back(dist);
        l->flush_on(flush_lvl);
    });

    auto logger = std::make_shared<spdlog::logger>(options.name, dist);
    logger->set_level(lvl);
    logger->flush_on(flush_lvl);
    spdlog::set_default_logger(logger);
    spdlog::set_level(lvl);
    if (options.flush_interval_s > 0) {
        spdlog::flush_every(std::chrono::seconds(options.flush_interval_s));
    }
}

Logger get() { return spdlog::default_logger(); }

Logger get(const std::string& name) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (auto existing = spdlog::get(name)) {
        return existing;
    }
    auto logger = make_logger_locked(name);
    spdlog::register_logger(logger);
    return logger;
}

void set_level(spdlog::level::level_enum lvl) { spdlog::set_level(lvl); }

void set_level(std::string_view lvl) { set_level(parse_level(lvl)); }

spdlog::level::level_enum level() { return spdlog::get_level(); }

void add_sink(const spdlog::sink_ptr& sink) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    ensure_dist_locked()->add_sink(sink);
}

void remove_sink(const spdlog::sink_ptr& sink) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.dist) s.dist->remove_sink(sink);
}

spdlog::sink_ptr set_console_sink(spdlog::sink_ptr sink) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto dist = ensure_dist_locked();
    auto previous = s.console;
    if (previous) dist->remove_sink(previous);
    if (sink) {
        sink->set_pattern(s.pattern);
        dist->add_sink(sink);
    }
    s.console = sink;
    return previous;
}

std::string pattern() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.pattern;
}

void flush() { spdlog::apply_all([](const Logger& l) { l->flush(); }); }

void shutdown() {
    flush();
    spdlog::shutdown();
}

}  // namespace forge::log
