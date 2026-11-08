#include "forge/log.hpp"

#include "forge/crash.hpp"
#include "forge/proto.hpp"
#include "forge/trace.hpp"

#include <spdlog/sinks/base_sink.h>
#include <deque>
#include <map>

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

// History + listeners + crash buffer: always installed.
class RecordSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::vector<v1::LogRecord> history(std::size_t limit) {
        std::lock_guard lock(mutex_);
        const std::size_t n = limit == 0 ? records_.size() : std::min(limit, records_.size());
        return {records_.end() - static_cast<std::ptrdiff_t>(n), records_.end()};
    }
    void set_max(std::size_t n) {
        std::lock_guard lock(mutex_);
        max_ = std::max<std::size_t>(n, 1);
        while (records_.size() > max_) records_.pop_front();
    }
    int add(Listener l) {
        std::lock_guard lock(listener_mutex_);
        listeners_[++next_] = std::move(l);
        return next_;
    }
    void remove(int id) {
        std::lock_guard lock(listener_mutex_);
        listeners_.erase(id);
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        v1::LogRecord r;
        r.set_time_unix_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(msg.time.time_since_epoch()).count());
        const auto lv = spdlog::level::to_string_view(msg.level);
        r.set_level(std::string(lv.data(), lv.size()));
        r.set_logger(std::string(msg.logger_name.data(), msg.logger_name.size()));
        r.set_message(std::string(msg.payload.data(), msg.payload.size()));
        r.set_thread(msg.thread_id);
        const auto ctx = trace::current();
        if (ctx.valid()) *r.mutable_trace() = trace::to_proto(ctx);
        // Crash report: the last lines, preformatted (the handler cannot allocate).
        spdlog::memory_buf_t line;
        formatter_->format(msg, line);
        crash::remember_log_line(line.data(), line.size());
        records_.push_back(r);
        while (records_.size() > max_) records_.pop_front();
        std::map<int, Listener> ls;
        {
            std::lock_guard lock(listener_mutex_);
            if (listeners_.empty()) return;
            ls = listeners_;
        }
        for (auto& [id, l] : ls) {
            try {
                l(r);
            } catch (...) {
            }
        }
    }
    void flush_() override {}

private:
    std::size_t max_ = 500;
    std::deque<v1::LogRecord> records_;
    std::mutex listener_mutex_;
    std::map<int, Listener> listeners_;
    int next_ = 0;
};

std::shared_ptr<RecordSink>& record_sink() {
    static auto s = std::make_shared<RecordSink>();
    return s;
}

// One JSON object per line, for log collectors.
class JsonFormatter final : public spdlog::formatter {
public:
    void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override {
        const auto lv = spdlog::level::to_string_view(msg.level);
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(msg.time.time_since_epoch()).count();
        const auto t = std::chrono::system_clock::to_time_t(msg.time);
        std::tm tm{};
        ::gmtime_r(&t, &tm);
        const auto micros = static_cast<unsigned>(((ns / 1000) % 1000000 + 1000000) % 1000000);  // 0..999999
        char stamp[64];
        std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02d.%06uZ", tm.tm_year + 1900, tm.tm_mon + 1,
                      tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, micros);
        nlohmann::json j{{"time", stamp},
                         {"level", std::string(lv.data(), lv.size())},
                         {"logger", std::string(msg.logger_name.data(), msg.logger_name.size())},
                         {"message", std::string(msg.payload.data(), msg.payload.size())},
                         {"thread", msg.thread_id}};
        const auto ctx = trace::current();
        if (ctx.valid()) j["trace"] = ctx.trace_hex();
        const auto text = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
        dest.append(text.data(), text.data() + text.size());
    }
    std::unique_ptr<spdlog::formatter> clone() const override { return std::make_unique<JsonFormatter>(); }
};

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

    if (options.format != "text" && options.format != "json") {
        throw std::invalid_argument("unknown log format '" + options.format + "' (text|json)");
    }
    const bool as_json = options.format == "json";
    auto styled = [&](const spdlog::sink_ptr& sink) {
        if (as_json) {
            sink->set_formatter(std::make_unique<JsonFormatter>());
        } else {
            sink->set_pattern(options.pattern);
        }
    };
    auto dist = std::make_shared<spdlog::sinks::dist_sink_mt>();
    record_sink()->set_max(options.history);
    record_sink()->set_pattern(options.pattern);
    dist->add_sink(record_sink());
    spdlog::sink_ptr console;
    if (options.console) {
        console = std::make_shared<spdlog::sinks::ansicolor_stderr_sink_mt>();
        styled(console);
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
        styled(file);
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

void set_logger_level(const std::string& logger, std::string_view lvl) {
    const auto l = parse_level(lvl);
    auto existing = spdlog::get(logger);
    if (!existing && spdlog::default_logger() && spdlog::default_logger()->name() == logger) {
        existing = spdlog::default_logger();
    }
    if (!existing) throw std::invalid_argument("unknown logger '" + logger + "'");
    existing->set_level(l);
}

std::vector<std::pair<std::string, std::string>> logger_levels() {
    std::vector<std::pair<std::string, std::string>> out;
    spdlog::apply_all([&](const Logger& l) {
        const auto lv = spdlog::level::to_string_view(l->level());
        out.emplace_back(l->name(), std::string(lv.data(), lv.size()));
    });
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<v1::LogRecord> history(std::size_t limit) { return record_sink()->history(limit); }
int add_listener(Listener listener) { return record_sink()->add(std::move(listener)); }
void remove_listener(int id) { record_sink()->remove(id); }

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
