#include "forge/log.hpp"

#include "forge/crash.hpp"
#include "forge/log_rt.hpp"
#include "forge/trace.hpp"

#include <nlohmann/json.hpp>
#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/Sink.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <pthread.h>
#include <csignal>
#include <stdexcept>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

namespace forge::log {

struct Logger::Impl {
    std::string name;
    detail::QuillLogger* quill = nullptr;
    std::atomic<int> flush_on{-1};  // Level, -1 = the global flush level
};

namespace {

using Frontend = quill::FrontendImpl<detail::FrontendOptions>;
constexpr const char* kReset = "\033[0m";

quill::LogLevel to_quill(Level level) {
    switch (level) {
        case Level::trace: return quill::LogLevel::TraceL1;
        case Level::debug: return quill::LogLevel::Debug;
        case Level::info: return quill::LogLevel::Info;
        case Level::warn: return quill::LogLevel::Warning;
        case Level::error: return quill::LogLevel::Error;
        case Level::critical: return quill::LogLevel::Critical;
        case Level::off: return quill::LogLevel::None;
    }
    return quill::LogLevel::Info;
}

Level from_quill(quill::LogLevel level) {
    switch (level) {
        case quill::LogLevel::TraceL3:
        case quill::LogLevel::TraceL2:
        case quill::LogLevel::TraceL1: return Level::trace;
        case quill::LogLevel::Debug: return Level::debug;
        case quill::LogLevel::Info:
        case quill::LogLevel::Notice: return Level::info;
        case quill::LogLevel::Warning: return Level::warn;
        case quill::LogLevel::Error: return Level::error;
        case quill::LogLevel::Critical: return Level::critical;
        case quill::LogLevel::None: return Level::off;
        default: return Level::info;
    }
}

int syslog_priority(Level level) {
    switch (level) {
        case Level::trace:
        case Level::debug: return LOG_DEBUG;
        case Level::info: return LOG_INFO;
        case Level::warn: return LOG_WARNING;
        case Level::error: return LOG_ERR;
        case Level::critical: return LOG_CRIT;
        case Level::off: return LOG_INFO;
    }
    return LOG_INFO;
}

// --- text lines -------------------------------------------------------------------
enum class Field { text, time, level, logger, message, thread, file, line };
struct Segment {
    Field field;
    std::string text;  // Field::text
};

std::vector<Segment> parse_pattern(const std::string& pattern) {
    static const std::map<std::string, Field> names{{"time", Field::time},     {"level", Field::level},
                                                    {"logger", Field::logger}, {"message", Field::message},
                                                    {"thread", Field::thread}, {"file", Field::file},
                                                    {"line", Field::line}};
    std::vector<Segment> out;
    std::string text;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{') {
            const auto close = pattern.find('}', i);
            if (close != std::string::npos) {
                auto it = names.find(pattern.substr(i + 1, close - i - 1));
                if (it == names.end()) {
                    throw std::invalid_argument("log pattern: unknown field '" + pattern.substr(i, close - i + 1) +
                                                "' (time, level, logger, message, thread, file, line)");
                }
                if (!text.empty()) out.push_back({Field::text, std::move(text)});
                text.clear();
                out.push_back({it->second, {}});
                i = close;
                continue;
            }
        }
        text += pattern[i];
    }
    if (!text.empty()) out.push_back({Field::text, std::move(text)});
    return out;
}

struct Entry {
    std::uint64_t time_ns;
    Level level;
    std::string_view logger;
    std::string_view message;
    std::string_view thread;
    std::string_view file;
    std::string_view line;
};

std::string local_time(std::uint64_t ns) {
    const auto t = static_cast<std::time_t>(ns / 1000000000ull);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec, (ns / 1000000ull) % 1000);
}

/// Text of an entry; with `color`, the level in its colour.
std::string render(const std::vector<Segment>& pattern, const Entry& e, bool color) {
    std::string out;
    out.reserve(e.message.size() + 64);
    for (const auto& s : pattern) {
        switch (s.field) {
            case Field::text: out += s.text; break;
            case Field::time: out += local_time(e.time_ns); break;
            case Field::level:
                if (color) out += level_color(e.level);
                out += short_name(e.level);
                if (color) out += kReset;
                break;
            case Field::logger: out += e.logger; break;
            case Field::message: out += e.message; break;
            case Field::thread: out += e.thread; break;
            case Field::file: out += e.file; break;
            case Field::line: out += e.line; break;
        }
    }
    out += '\n';
    return out;
}

std::string render_json(const Entry& e, std::string_view trace_hex) {
    const auto secs = static_cast<std::time_t>(e.time_ns / 1000000000ull);
    std::tm tm{};
    ::gmtime_r(&secs, &tm);
    nlohmann::json j{{"time", std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:06}Z", tm.tm_year + 1900, tm.tm_mon + 1,
                                          tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (e.time_ns / 1000ull) % 1000000)},
                     {"level", to_string(e.level)},
                     {"logger", e.logger},
                     {"message", e.message},
                     {"thread", std::strtoull(std::string(e.thread).c_str(), nullptr, 10)}};
    if (!trace_hex.empty()) j["trace"] = trace_hex;
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

void write_fd(int fd, std::string_view text) {
    while (!text.empty()) {
        const auto n = ::write(fd, text.data(), text.size());
        if (n <= 0) return;
        text.remove_prefix(static_cast<std::size_t>(n));
    }
}

/// stderr goes to the journal (systemd sets JOURNAL_STREAM to its device:inode).
bool stderr_is_journal() {
    const char* js = std::getenv("JOURNAL_STREAM");
    if (!js) return false;
    struct stat st {};
    if (::fstat(STDERR_FILENO, &st) != 0) return false;
    return std::format("{}:{}", st.st_dev, st.st_ino) == js;
}

std::array<std::uint8_t, 32> from_hex(std::string_view hex, std::size_t& n) {
    std::array<std::uint8_t, 32> out{};
    n = 0;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (std::size_t i = 0; i + 1 < hex.size() && n < out.size(); i += 2) {
        const int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return out;
}

// --- the one sink of every logger (runs on the logging background thread) --------------
class Dispatch final : public quill::Sink {
public:
    struct Config {
        bool console = true;
        bool color = false;
        bool journal = false;
        bool json = false;
        std::vector<Segment> pattern = parse_pattern(Options{}.pattern);
        std::string file;
        std::size_t max_size = 10u * 1024 * 1024;
        std::size_t max_files = 3;
        bool syslog = false;
        std::string syslog_ident;
        Level flush_level = Level::warn;
        std::size_t history = 500;
    };

    void configure(Config c) {
        std::lock_guard lock(mutex_);
        if (file_ && c.file != config_.file) {
            std::fclose(file_);
            file_ = nullptr;
        }
        if (config_.syslog) ::closelog();
        config_ = std::move(c);
        if (config_.syslog) ::openlog(config_.syslog_ident.c_str(), LOG_PID, LOG_DAEMON);
        if (!config_.file.empty() && !file_) open_file();
        while (records_.size() > std::max<std::size_t>(config_.history, 1)) records_.pop_front();
    }

    TerminalWriter set_writer(TerminalWriter w) {
        std::lock_guard lock(mutex_);
        std::swap(writer_, w);
        return w;
    }

    std::vector<Record> history(std::size_t limit) {
        std::lock_guard lock(mutex_);
        const std::size_t n = limit == 0 ? records_.size() : std::min(limit, records_.size());
        return {records_.end() - static_cast<std::ptrdiff_t>(n), records_.end()};
    }
    Subscription add(std::function<void(const Record&)> listener) {
        // On the logging thread nothing can log about a listener that throws: stderr it is.
        return listeners_.add([l = std::move(listener)](const Record& r) {
            try {
                l(r);
            } catch (const std::exception& e) {
                write_fd(STDERR_FILENO, std::string("forge log: a listener failed: ") + e.what() + "\n");
            }
        });
    }

    void write_log(quill::MacroMetadata const* metadata, std::uint64_t timestamp, std::string_view thread_id,
                   std::string_view, std::string const&, std::string_view logger_name, quill::LogLevel level,
                   std::string_view, std::string_view, std::vector<std::pair<std::string, std::string>> const*,
                   std::string_view message, std::string_view) override {
        const std::string_view tags = metadata && metadata->tags() ? metadata->tags() : "";
        const std::string_view line_no = metadata && metadata->line() ? metadata->line() : "";
        const Entry e{timestamp, from_quill(level), logger_name, message, thread_id,
                      metadata ? metadata->file_name() : std::string_view(), line_no};

        // tags: "<trace id hex>:<span id hex>" of the caller (see Logger::write)
        trace::Context ctx;
        std::string_view trace_hex;
        if (const auto colon = tags.find(':'); colon != std::string_view::npos) {
            std::size_t n = 0;
            const auto t = from_hex(tags.substr(0, colon), n);
            if (n == ctx.trace_id.size()) std::copy_n(t.begin(), n, ctx.trace_id.begin());
            const auto sp = from_hex(tags.substr(colon + 1), n);
            if (n == ctx.span_id.size()) std::copy_n(sp.begin(), n, ctx.span_id.begin());
            trace_hex = tags.substr(0, colon);
        }

        Record record;
        record.time_unix_ns = static_cast<std::int64_t>(timestamp);
        record.level = e.level;
        record.logger = std::string(logger_name);
        record.message = std::string(message);
        record.thread = std::strtoull(std::string(thread_id).c_str(), nullptr, 10);
        record.trace = ctx;

        {
            std::lock_guard lock(mutex_);
            const std::string plain = config_.json ? render_json(e, trace_hex) : render(config_.pattern, e, false);
            if (config_.console) {
                if (writer_) {
                    writer_(plain, config_.json ? plain : render(config_.pattern, e, true));
                } else if (config_.journal) {
                    // journald: the priority as a prefix, no time (it adds its own)
                    write_fd(STDERR_FILENO, std::format("<{}>[{}] [{}] {}\n", syslog_priority(e.level),
                                                        short_name(e.level), logger_name, message));
                } else {
                    write_fd(STDERR_FILENO, config_.color && !config_.json ? render(config_.pattern, e, true) : plain);
                }
            }
            if (file_) {
                std::fwrite(plain.data(), 1, plain.size(), file_);
                file_size_ += plain.size();
                if (e.level >= config_.flush_level) std::fflush(file_);
                if (file_size_ >= config_.max_size) rotate();
            }
            if (config_.syslog) {
                ::syslog(syslog_priority(e.level), "[%.*s] %.*s", static_cast<int>(logger_name.size()),
                         logger_name.data(), static_cast<int>(message.size()), message.data());
            }
            crash::remember_log_line(plain.data(), plain.size());  // the crash report's last lines
            records_.push_back(record);
            while (records_.size() > std::max<std::size_t>(config_.history, 1)) records_.pop_front();
        }

        listeners_.notify(record);
    }

    void flush_sink() override {
        std::lock_guard lock(mutex_);
        if (file_) std::fflush(file_);
    }

private:
    void open_file() {
        const std::filesystem::path path(config_.file);
        std::error_code ec;
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
        file_ = std::fopen(config_.file.c_str(), "a");
        file_size_ = file_ ? static_cast<std::size_t>(std::filesystem::file_size(path, ec)) : 0;
        if (ec) file_size_ = 0;
    }

    // app.log -> app.log.1 -> ... -> app.log.<max_files> (the oldest is dropped)
    void rotate() {
        std::fclose(file_);
        file_ = nullptr;
        std::error_code ec;
        const std::size_t keep = std::max<std::size_t>(config_.max_files, 1);
        std::filesystem::remove(config_.file + "." + std::to_string(keep), ec);
        for (std::size_t i = keep; i > 1; --i) {
            std::filesystem::rename(config_.file + "." + std::to_string(i - 1), config_.file + "." + std::to_string(i),
                                    ec);
        }
        std::filesystem::rename(config_.file, config_.file + ".1", ec);
        open_file();
    }

    std::mutex mutex_;
    Config config_;
    TerminalWriter writer_;
    std::FILE* file_ = nullptr;
    std::size_t file_size_ = 0;
    std::deque<Record> records_;
    Listeners<Record> listeners_;
};

// --- loggers --------------------------------------------------------------------------
struct State {
    std::mutex mutex;
    std::shared_ptr<Dispatch> sink = std::make_shared<Dispatch>();
    std::map<std::string, std::shared_ptr<Logger::Impl>> loggers;
    std::shared_ptr<Logger::Impl> fallback;  // default logger
    std::atomic<int> level{static_cast<int>(Level::info)};
    std::atomic<int> flush_level{static_cast<int>(Level::warn)};
    std::chrono::seconds flush_interval{2};
    bool backend = false;
    bool configured = false;
};

State& state() {
    static State* s = new State;  // never destroyed: logging may happen during static destruction
    return *s;
}

// Must be called with state().mutex held.
void ensure_backend_locked(State& s) {
    if (s.backend) return;
    quill::BackendOptions o;
    o.thread_name = "forge-log";
    o.sleep_duration = std::chrono::milliseconds(1);
    o.sink_min_flush_interval = std::chrono::duration_cast<std::chrono::milliseconds>(s.flush_interval);
    o.check_printable_char = {};  // messages as they are (escape sequences, tabs, UTF-8)
    // Quill's own notices: a queue that grew is routine; dropped messages and errors are not.
    o.error_notifier = [](const std::string& message) {
        if (message.find("Allocated a new SPSC queue") != std::string::npos) return;
        write_fd(STDERR_FILENO, "forge log: " + message + "\n");
    };
    // The thread inherits the signal mask of its creator. Asynchronous signals
    // (SIGTERM, SIGINT, SIGHUP, ...) belong to the threads that wait for them, not
    // to this one, even when logging starts before the application blocks them;
    // faults (SIGSEGV, ...) stay deliverable for the crash handler.
    sigset_t all, previous;
    ::sigfillset(&all);
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP, SIGSYS}) ::sigdelset(&all, sig);
    ::pthread_sigmask(SIG_BLOCK, &all, &previous);
    quill::Backend::start(o);
    ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    s.backend = true;
    if (!s.configured) {  // logging before init(): stderr, colours on a terminal
        Dispatch::Config c;
        c.journal = stderr_is_journal();
        c.color = !c.journal && ::isatty(STDERR_FILENO) && !std::getenv("NO_COLOR");
        s.sink->configure(std::move(c));
    }
}

// Must be called with state().mutex held.
std::shared_ptr<Logger::Impl> make_logger_locked(State& s, const std::string& name) {
    ensure_backend_locked(s);
    auto it = s.loggers.find(name);
    if (it != s.loggers.end()) return it->second;
    auto impl = std::make_shared<Logger::Impl>();
    impl->name = name;
    impl->quill = Frontend::create_or_get_logger(
        name, std::static_pointer_cast<quill::Sink>(s.sink),
        quill::PatternFormatterOptions{"%(message)", "%H:%M:%S", quill::Timezone::LocalTime, false},
        quill::ClockSourceType::System);
    impl->quill->set_log_level(to_quill(static_cast<Level>(s.level.load())));
    s.loggers.emplace(name, impl);
    return impl;
}

std::shared_ptr<Logger::Impl> default_impl() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.fallback) s.fallback = make_logger_locked(s, "app");
    return s.fallback;
}

}  // namespace

// --- levels -----------------------------------------------------------------------------
Level parse_level(std::string_view level) {
    std::string l(level);
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return std::tolower(c); });
    if (l == "trace") return Level::trace;
    if (l == "debug") return Level::debug;
    if (l == "info") return Level::info;
    if (l == "warn" || l == "warning") return Level::warn;
    if (l == "error" || l == "err") return Level::error;
    if (l == "critical" || l == "fatal") return Level::critical;
    if (l == "off" || l == "none") return Level::off;
    throw std::invalid_argument("unknown log level '" + std::string(level) +
                                "' (expected trace|debug|info|warn|error|critical|off)");
}

std::string_view to_string(Level level) {
    switch (level) {
        case Level::trace: return "trace";
        case Level::debug: return "debug";
        case Level::info: return "info";
        case Level::warn: return "warning";
        case Level::error: return "error";
        case Level::critical: return "critical";
        case Level::off: return "off";
    }
    return "info";
}

std::string_view short_name(Level level) {
    switch (level) {
        case Level::trace: return "TRC";
        case Level::debug: return "DBG";
        case Level::info: return "INF";
        case Level::warn: return "WRN";
        case Level::error: return "ERR";
        case Level::critical: return "CRT";
        case Level::off: return "OFF";
    }
    return "INF";
}

std::string_view level_color(Level level) {
    switch (level) {
        case Level::trace: return "\033[37m";
        case Level::debug: return "\033[36m";
        case Level::info: return "\033[32m";
        case Level::warn: return "\033[33m\033[1m";
        case Level::error: return "\033[31m\033[1m";
        case Level::critical: return "\033[1m\033[41m";
        case Level::off: return "";
    }
    return "";
}

// --- Logger -------------------------------------------------------------------------------
Logger::Logger() : impl_(default_impl()) {}

const std::string& Logger::name() const { return impl_->name; }

Level Logger::level() const { return from_quill(impl_->quill->get_log_level()); }

void Logger::set_level(Level level) const { impl_->quill->set_log_level(to_quill(level)); }

void Logger::flush_on(Level level) const { impl_->flush_on = static_cast<int>(level); }

void Logger::flush() const { impl_->quill->flush_log(); }

void Logger::write(Level level, std::string_view message, const std::source_location& where) const {
    if (!should_log(level)) return;
    // The caller's trace context travels in the tags (the sink runs on another thread).
    std::string tags;
    if (const auto ctx = ::forge::trace::current(); ctx.valid()) tags = ctx.trace_hex() + ":" + ctx.span_hex();
    QUILL_LOG_RUNTIME_METADATA_DEEP(impl_->quill, to_quill(level), where.file_name(),
                                    static_cast<std::uint32_t>(where.line()), where.function_name(), tags.c_str(),
                                    "{}", message);
    const int flush_on = impl_->flush_on.load();
    if (static_cast<int>(level) >= (flush_on >= 0 ? flush_on : state().flush_level.load())) quill::Backend::notify();
}

namespace detail {
QuillLogger* quill_logger(const Logger& logger) { return logger.impl()->quill; }
}  // namespace detail

// --- global --------------------------------------------------------------------------------
void init(const Options& options) {
    const auto lvl = parse_level(options.level);
    const auto flush_lvl = parse_level(options.flush_level);
    if (options.format != "text" && options.format != "json") {
        throw std::invalid_argument("unknown log format '" + options.format + "' (text|json)");
    }
    if (options.color != "auto" && options.color != "always" && options.color != "never") {
        throw std::invalid_argument("unknown log colour mode '" + options.color + "' (auto|always|never)");
    }
    Dispatch::Config c;
    c.console = options.console;
    c.journal = stderr_is_journal();
    c.color = options.color == "always" ||
              (options.color == "auto" && !c.journal && ::isatty(STDERR_FILENO) && !std::getenv("NO_COLOR"));
    c.json = options.format == "json";
    c.pattern = parse_pattern(options.pattern);
    c.file = options.file;
    c.max_size = std::max<std::size_t>(options.max_size_mb, 1) * 1024 * 1024;
    c.max_files = std::max<std::size_t>(options.max_files, 1);
    c.syslog = options.syslog;
    c.syslog_ident = options.name;
    c.flush_level = flush_lvl;
    c.history = options.history;

    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.backend && options.flush_interval_s > 0) s.flush_interval = std::chrono::seconds(options.flush_interval_s);
    s.configured = true;
    ensure_backend_locked(s);
    s.sink->configure(std::move(c));
    s.flush_level = static_cast<int>(flush_lvl);
    s.level = static_cast<int>(lvl);
    for (auto& [name, l] : s.loggers) l->quill->set_log_level(to_quill(lvl));
    s.fallback = make_logger_locked(s, options.name);
}

Logger get() { return Logger(default_impl()); }

Logger get(const std::string& name) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return Logger(make_logger_locked(s, name));
}

void set_level(Level level) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.level = static_cast<int>(level);
    for (auto& [name, l] : s.loggers) l->quill->set_log_level(to_quill(level));
}

void set_level(std::string_view level) { set_level(parse_level(level)); }

Level level() { return static_cast<Level>(state().level.load()); }

void set_logger_level(const std::string& logger, std::string_view level) {
    const auto l = parse_level(level);
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto it = s.loggers.find(logger);
    if (it == s.loggers.end()) throw std::invalid_argument("unknown logger '" + logger + "'");
    it->second->quill->set_log_level(to_quill(l));
}

std::vector<std::pair<std::string, std::string>> logger_levels() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& [name, l] : s.loggers) {
        out.emplace_back(name, std::string(to_string(from_quill(l->quill->get_log_level()))));
    }
    return out;  // std::map: sorted by name
}

std::vector<Record> history(std::size_t limit) { return state().sink->history(limit); }
Subscription add_listener(std::function<void(const Record&)> listener) {
    return state().sink->add(std::move(listener));
}

TerminalWriter set_terminal_writer(TerminalWriter writer) {
    flush();  // what is queued goes out the old way
    return state().sink->set_writer(std::move(writer));
}

void flush() {
    // Each thread has its own queue: this waits for what the calling thread logged
    // (the background thread empties the other queues meanwhile).
    get().flush();
}

void shutdown() { flush(); }

}  // namespace forge::log
