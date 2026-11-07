#include "forge/application.hpp"

#include "forge/console.hpp"
#include "forge/control.hpp"
#include "forge/crash.hpp"
#include "forge/http.hpp"
#include "forge/proto.hpp"
#include "forge/rt.hpp"
#include "forge/systemd.hpp"
#include "forge/telnet.hpp"
#include "forge/hub.hpp"
#include "forge/trace.hpp"
#include "forge/version.hpp"
#include "forge/web.hpp"
#include "forge/zmq.hpp"

#include <fstream>
#include <sys/resource.h>

#include <CLI/CLI.hpp>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <iostream>
#include <pthread.h>
#include <unistd.h>

namespace forge {

namespace detail {

namespace {
enum LoopErrorKind { kNone, kHttp, kConfig, kInvalidArgument, kOutOfRange, kRuntime, kStopped };
}

void LoopError::capture() noexcept {
    try {
        throw;
    } catch (const http::HttpError& e) {
        kind = kHttp;
        status = e.status();
        message = e.what();
        body = e.body();
    } catch (const ConfigError& e) {
        kind = kConfig;
        message = e.what();
    } catch (const std::invalid_argument& e) {
        kind = kInvalidArgument;
        message = e.what();
    } catch (const std::out_of_range& e) {
        kind = kOutOfRange;
        message = e.what();
    } catch (const std::exception& e) {
        kind = kRuntime;
        message = e.what();
    } catch (...) {
        kind = kRuntime;
        message = "unknown exception";
    }
}

void LoopError::set_stopped() {
    kind = kStopped;
    message = "the application stopped before the task ran";
}

void LoopError::rethrow() const {
    switch (kind) {
        case kHttp: throw http::HttpError(status, message, body);
        case kConfig: throw ConfigError(message);
        case kInvalidArgument: throw std::invalid_argument(message);
        case kOutOfRange: throw std::out_of_range(message);
        default: throw std::runtime_error(message);
    }
}

}  // namespace detail
namespace {

constexpr int kHandledSignals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};

sigset_t handled_signal_set() {
    sigset_t set;
    sigemptyset(&set);
    for (int sig : kHandledSignals) sigaddset(&set, sig);
    return set;
}

std::string format_duration(std::chrono::steady_clock::duration d) {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(d).count();
    const auto days = secs / 86400;
    secs %= 86400;
    return days > 0 ? fmt::format("{}d {:02}:{:02}:{:02}", days, secs / 3600, (secs % 3600) / 60, secs % 60)
                    : fmt::format("{:02}:{:02}:{:02}", secs / 3600, (secs % 3600) / 60, secs % 60);
}

// Keeps the latest log lines for the dashboard and streams new ones to it.
class WebLogSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    WebLogSink(std::shared_ptr<http::EventStream> stream, std::size_t history)
        : stream_(std::move(stream)), max_(std::max<std::size_t>(history, 1)) {}

    json history() {
        std::lock_guard lock(mutex_);
        return json(std::vector<json>(history_.begin(), history_.end()));
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        const auto t = std::chrono::system_clock::to_time_t(msg.time);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(msg.time.time_since_epoch()).count() % 1000;
        std::tm tm{};
        ::localtime_r(&t, &tm);
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec,
                      static_cast<int>(ms));
        json entry{{"time", stamp},
                   {"level", std::string(spdlog::level::to_string_view(msg.level).data(),
                                         spdlog::level::to_string_view(msg.level).size())},
                   {"logger", std::string(msg.logger_name.data(), msg.logger_name.size())},
                   {"message", std::string(msg.payload.data(), msg.payload.size())}};
        history_.push_back(entry);
        while (history_.size() > max_) history_.pop_front();
        stream_->publish("log", entry);
    }
    void flush_() override {}

private:
    std::shared_ptr<http::EventStream> stream_;
    std::size_t max_;
    std::deque<json> history_;
};

std::string host_name() {
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) != 0) return {};
    return buf;
}

// Caller of a console command: the local terminal (admin) or a telnet session.
Caller console_caller() {
    Caller c;
    if (const auto* s = Console::current_session()) {
        c.channel = s->channel;
        c.client = s->client;
        c.user = s->user;
        c.role = s->role;
    } else {
        c.channel = "console";
    }
    return c;
}

bool is_loopback(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

}  // namespace

Application::Application(AppInfo info, std::string installed_config)
    : info_(std::move(info)), cli_(std::make_unique<CLI::App>()) {
    if (info_.name.empty()) info_.name = "app";
    if (info_.config_paths.empty()) {
        info_.config_paths.emplace_back(info_.name + ".toml");
        if (!installed_config.empty()) info_.config_paths.emplace_back(installed_config);
        const auto etc = std::filesystem::path("/etc") / info_.name / (info_.name + ".toml");
        if (installed_config != etc.string()) info_.config_paths.push_back(etc);
    }

    auto& cli = *cli_;
    cli.name(info_.name);
    cli.description(info_.description.empty() ? info_.name : info_.description);
    cli.set_version_flag("-V,--version",
                         fmt::format("{} {} (forgecpp {})", info_.name, info_.version, version_string));
    cli.option_defaults()->group("Framework options");

    cli.add_option("-c,--config", config_file_, "Configuration file (TOML)");
    cli.add_option("-D,--set", overrides_, "Override a config value, e.g. -D log.level=debug")
        ->type_name("KEY=VALUE")
        ->allow_extra_args(false);
    cli.add_option("-l,--log-level", log_level_, "Log level: trace|debug|info|warn|error|critical|off");
    cli.add_flag("-v,--verbose", verbosity_, "Increase verbosity (-v debug, -vv trace)");
    cli.add_option("--log-file", log_file_, "Also log to this (rotating) file");
    cli.add_flag("--syslog", syslog_, "Also log to syslog");
    cli.add_flag("-i,--interactive", interactive_, "Start the interactive console");
    cli.add_option("--http", http_listen_, "Enable the HTTP server on [HOST:]PORT")->type_name("[HOST:]PORT");
    cli.add_flag("--print-config", print_config_, "Print the effective configuration as JSON and exit");
    cli.add_flag("--help-config", help_config_, "Describe every configuration key and exit");
    cli.add_flag("--check-config", check_config_, "Validate the configuration and exit (status 0 = valid)");

    cli.option_defaults()->group("Options");
}

Application::~Application() {
    // Normally torn down by run(); make sure nothing outlives the object.
    if (telnet_) telnet_->stop();
    if (console_) console_->stop();
    if (http_) http_->stop();
    if (scheduler_) scheduler_->stop();
    stop_signal_thread();
}

// ---------------------------------------------------------------------------
// public API

void Application::stop(int exit_code) {
    if (!stop_requested_.exchange(true)) {
        exit_code_ = exit_code;
    }
    tasks_.push([] {});  // wake the main loop
}

void Application::reload() {
    post([this] { do_reload(); });
}

void Application::post(Task task) {
    if (!task) return;
    // Main loop lag: time from post() to execution (a busy loop shows up here first).
    static auto& lag = metrics::registry().histogram("forge.loop.lag_seconds", metrics::latency_buckets(),
                                                     "Delay between post() and execution on the main loop", "s");
    static auto& busy = metrics::registry().counter("forge.loop.busy_us", "Time spent running main loop tasks", "us");
    tasks_.push([t = std::move(task), queued = std::chrono::steady_clock::now()] {
        const auto start = std::chrono::steady_clock::now();
        lag.observe(std::chrono::duration<double>(start - queued).count());
        FORGE_TRACE_SCOPE("loop.task", "forge");
        t();
        busy.inc(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()));
    });
}

Scheduler::Id Application::after(Duration delay, Task task) {
    return scheduler().after(delay, std::move(task));
}

Scheduler::Id Application::every(Duration period, Task task, bool run_immediately) {
    return scheduler().every(period, std::move(task), run_immediately);
}

bool Application::cancel(Scheduler::Id id) { return scheduler_ ? scheduler_->cancel(id) : false; }

Config Application::config() const {
    std::lock_guard lock(config_mutex_);
    return config_;
}

Logger Application::logger() const { return logger_ ? logger_ : log::get(); }

ThreadPool& Application::pool() {
    if (!pool_) throw std::logic_error("Application::pool() used before run()");
    return *pool_;
}

Scheduler& Application::scheduler() {
    if (!scheduler_) throw std::logic_error("Application::scheduler() used before run()");
    return *scheduler_;
}

std::chrono::steady_clock::duration Application::uptime() const {
    return std::chrono::steady_clock::now() - started_at_;
}

Application& Application::hook_start(Hook hook) {
    hooks_.start.push_back(std::move(hook));
    return *this;
}
Application& Application::hook_stop(Hook hook) {
    hooks_.stop.push_back(std::move(hook));
    return *this;
}
Application& Application::hook_reload(Hook hook) {
    hooks_.reload.push_back(std::move(hook));
    return *this;
}
Application& Application::hook_http(std::function<void(Application&, http::Server&)> hook) {
    hooks_.http.push_back(std::move(hook));
    return *this;
}
Application& Application::hook_web(std::function<void(Application&, web::Dashboard&)> hook) {
    hooks_.web.push_back(std::move(hook));
    return *this;
}
Application& Application::hook_console(std::function<void(Application&, Console&)> hook) {
    hooks_.console.push_back(std::move(hook));
    return *this;
}

// default virtuals
void Application::setup_cli(CLI::App&) {}
void Application::setup_config(config::Schema&) {}
void Application::setup_http(http::Server&) {}
void Application::setup_console(Console&) {}
void Application::setup_web(web::Dashboard&) {}

void Application::publish_web(const std::string& event, const json& data) {
    if (web_events_) web_events_->publish("app", json{{"event", event}, {"data", data}});
}

void Application::notify_web(const std::string& message, const std::string& level) {
    publish_web("notify", json{{"message", message}, {"level", level}});
}
void Application::on_start() {}
void Application::on_stop() {}
void Application::on_reload(const Config&) {}

// ---------------------------------------------------------------------------
// lifecycle

int Application::run(int argc, char** argv) {
    started_at_ = std::chrono::steady_clock::now();
    main_thread_ = std::this_thread::get_id();

    // Block the handled signals before any thread is created so that every
    // thread inherits the mask and only the signal thread receives them.
    const sigset_t set = handled_signal_set();
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    if (int rc = parse_command_line(argc, argv); rc >= 0) return rc;

    // Declared configuration: framework keys, the application's, its components'.
    declare_framework_config(schema_);
    setup_config(schema_);
    for (auto& c : components_) c->setup_config(schema_);
    if (help_config_) {
        std::cout << "# " << info_.name << " configuration keys (TOML)\n" << schema_.help();
        return 0;
    }

    Config cfg;
    try {
        cfg = load_config();
    } catch (const std::exception& e) {
        std::cerr << info_.name << ": " << e.what() << "\n";
        return 1;
    }
    {
        std::lock_guard lock(config_mutex_);
        config_ = cfg;
    }

    if (print_config_) {
        std::cout << cfg.to_json().dump(2) << "\n";
        return 0;
    }
    if (!validate_config(cfg, true)) return 1;
    if (check_config_) {
        std::cout << info_.name << ": configuration is valid\n";
        return 0;
    }
    proto::set_source(info_.name);
    metrics::registry().set_labels({{"app", info_.name}});

    try {
        setup_logging(cfg);
    } catch (const std::exception& e) {
        std::cerr << info_.name << ": invalid logging configuration: " << e.what() << "\n";
        return 1;
    }

    int rc = 0;
    try {
        const auto workers = cfg.get_or<int>("threads.workers", 0);
        pool_ = std::make_unique<ThreadPool>(workers > 0 ? static_cast<std::size_t>(workers)
                                                         : std::thread::hardware_concurrency());
        scheduler_ = std::make_unique<Scheduler>([this](Scheduler::Task t) { post(std::move(t)); });
        start_signal_thread();

        logger_->info("{} {} starting (forgecpp {}, pid {}, {} worker threads)", info_.name, info_.version,
                      version_string, ::getpid(), pool_->get_thread_count());
        if (!cfg.source().empty()) logger_->info("configuration: {}", cfg.source().string());
        trace::set_thread_name("main");

        auth_.load(cfg);
        start_observability(cfg);
        control_ = std::make_unique<ControlService>(*this);

        // The control registry exists even without HTTP: the console and
        // other channels (ZeroMQ, forgectl, telnet) use it too.
        dashboard_ = std::make_unique<web::Dashboard>(cfg.get_or<std::string>("web.title", info_.name));
        setup_web(*dashboard_);
        for (auto& c : components_) c->setup_web(*dashboard_);
        for (auto& h : hooks_.web) h(*this, *dashboard_);
        restore_state();

        create_http(cfg);
        create_console(cfg);
        start_messaging(cfg);
        start_telnet(cfg);

        on_start();
        for (auto& c : components_) c->on_start();
        for (auto& h : hooks_.start) h(*this);

        if (http_) {
            const auto host = cfg.get_or<std::string>("http.host", "0.0.0.0");
            const auto port = cfg.get_or<int>("http.port", 8080);
            http_->start(host, port);
        }
        if (web_events_) {
            const auto refresh = std::max(cfg.get_or<int>("web.refresh_ms", 500), 50);
            every(std::chrono::milliseconds(refresh), [this] { push_web_state(); });
            logger_->info("web dashboard: http://{}:{}/forge/",
                          is_loopback(cfg.get_or<std::string>("http.host", "0.0.0.0")) ? "127.0.0.1" : host_name(),
                          http_->port());
        }
        if (console_ && cfg.get_or<bool>("console.enabled", false)) console_->start();

        if (auto wd = systemd::watchdog_interval()) {
            every(*wd / 2, [this] { watchdog_tick(); }, true);
            logger_->debug("systemd watchdog enabled ({} ms)",
                           std::chrono::duration_cast<std::chrono::milliseconds>(*wd).count());
        }
        const auto steady_ms = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        };
        loop_tick_ms_ = steady_ms();
        every(std::chrono::seconds(1), [this, steady_ms] {
            loop_tick_ms_ = steady_ms();  // /health reports a stalled main loop
            save_state();
            if (state_event_pending_.exchange(false)) control_changed({}, false);
        });
        start_hub_client(cfg);
        systemd::notify("READY=1\nSTATUS=running");
        logger_->info("{} started", info_.name);

        main_loop();
        rc = exit_code_.load();
    } catch (const std::exception& e) {
        if (logger_) {
            logger_->critical("fatal: {}", e.what());
        } else {
            std::cerr << info_.name << ": fatal: " << e.what() << "\n";
        }
        rc = 1;
    }

    shutdown();
    return rc;
}

int Application::parse_command_line(int argc, char** argv) {
    setup_cli(*cli_);
    try {
        cli_->parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return cli_->exit(e);
    }
    return -1;
}

Config Application::load_config() const {
    Config cfg;
    if (!config_file_.empty()) {
        cfg = Config::load_file(config_file_);
    } else {
        for (const auto& candidate : info_.config_paths) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec)) {
                cfg = Config::load_file(candidate);
                break;
            }
        }
    }

    for (const auto& o : overrides_) cfg.apply_override(o);

    // Explicit flags win over the file and -D overrides.
    if (verbosity_ == 1) cfg.set("log.level", "debug");
    if (verbosity_ >= 2) cfg.set("log.level", "trace");
    if (!log_level_.empty()) cfg.set("log.level", log_level_);
    if (!log_file_.empty()) cfg.set("log.file", log_file_);
    if (syslog_) cfg.set("log.syslog", true);
    if (interactive_) cfg.set("console.enabled", true);
    if (!http_listen_.empty()) {
        cfg.set("http.enabled", true);
        const auto colon = http_listen_.rfind(':');
        const std::string port = colon == std::string::npos ? http_listen_ : http_listen_.substr(colon + 1);
        if (colon != std::string::npos && colon > 0) cfg.set("http.host", http_listen_.substr(0, colon));
        try {
            cfg.set("http.port", static_cast<std::int64_t>(std::stoi(port)));
        } catch (const std::exception&) {
            throw ConfigError("invalid --http value '" + http_listen_ + "'");
        }
    }
    return cfg;
}

void Application::setup_logging(const Config& cfg) {
    log::Options o;
    o.format = cfg.get_or<std::string>("log.format", o.format);
    o.history = cfg.get_or<std::size_t>("log.history", o.history);
    o.name = info_.name;
    o.level = cfg.get_or<std::string>("log.level", o.level);
    o.pattern = cfg.get_or<std::string>("log.pattern", o.pattern);
    o.console = cfg.get_or<bool>("log.console", o.console);
    o.file = cfg.get_or<std::string>("log.file", o.file);
    o.max_size_mb = cfg.get_or<std::size_t>("log.max_size_mb", o.max_size_mb);
    o.max_files = cfg.get_or<std::size_t>("log.max_files", o.max_files);
    o.syslog = cfg.get_or<bool>("log.syslog", o.syslog);
    o.flush_level = cfg.get_or<std::string>("log.flush_level", o.flush_level);
    o.flush_interval_s = cfg.get_or<int>("log.flush_interval_s", o.flush_interval_s);
    log::init(o);
    logger_ = log::get();
}

void Application::start_signal_thread() {
    signal_thread_running_ = true;
    signal_thread_ = std::thread([this] {
        const sigset_t set = handled_signal_set();
        const timespec timeout{0, 200 * 1000 * 1000};
        while (signal_thread_running_.load()) {
            const int sig = ::sigtimedwait(&set, nullptr, &timeout);
            if (sig < 0) continue;  // timeout / EINTR
            if (sig == SIGHUP) {
                logger_->info("SIGHUP received, reloading configuration");
                reload();
            } else {
                logger_->info("{} received, shutting down", ::strsignal(sig));
                stop_signal_ = sig;
                if (stopping()) {
                    // Second signal while shutting down: give up waiting.
                    logger_->warn("forced exit");
                    log::flush();
                    std::_Exit(128 + sig);
                }
                stop(0);
            }
        }
    });
}

void Application::stop_signal_thread() {
    signal_thread_running_ = false;
    if (signal_thread_.joinable()) signal_thread_.join();
}

void Application::create_http(const Config& cfg) {
    if (!cfg.get_or<bool>("http.enabled", false)) return;
    http_ = std::make_unique<http::Server>();
    http_->threads(static_cast<std::size_t>(std::max(cfg.get_or<int>("http.threads", 8), 2)));

    // Credentials: [auth] users (roles are checked per request by ControlService).
    // Anonymous callers get [auth].default_role, unless [auth].anonymous = false.
    if (auth_.enabled() && !cfg.get_or<bool>("auth.anonymous", true)) {
        http::Auth auth;
        auth.check = [this](const std::string& header) { return auth_.by_http_header(header).has_value(); };
        auth.public_paths.push_back("/metrics");
        http_->auth(auth);
    } else if (!auth_.enabled() && !is_loopback(cfg.get_or<std::string>("http.host", "0.0.0.0"))) {
        logger_->warn("http: no [auth] configured - anyone who can reach port {} can use the API",
                      cfg.get_or<int>("http.port", 8080));
    }

    http_->get("/health", [this](const http::Request& req, http::Response& res) {
        const auto h = health_.snapshot();
        json components = json::array();
        for (const auto& c : h.components()) {
            components.push_back({{"name", c.name()},
                                  {"state", to_string(static_cast<HealthState>(c.state()))},
                                  {"message", c.message()},
                                  {"critical", c.critical()}});
        }
        const auto overall = health_.overall();
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
        const auto loop_age = loop_tick_ms_.load() > 0 ? now_ms - loop_tick_ms_.load() : 0;
        // The 1 s heartbeat runs on the main loop: an old one means the loop is stuck.
        const char* status = stopping() ? "stopping" : (loop_age > 5000 ? "stalled" : "ok");
        json j{{"status", status},
               {"pid", ::getpid()},
               {"loop_age_ms", loop_age},
               {"health", to_string(overall)},
               {"components", std::move(components)},
               {"name", info_.name},
               {"version", info_.version},
               {"uptime_s", std::chrono::duration_cast<std::chrono::seconds>(uptime()).count()},
               {"alarms", alarms_.summary()}};
        // ?strict=1: 503 while a critical component has failed (load balancers, probes).
        res.status = (req.has_param("strict") && overall == HealthState::failed) ? 503 : 200;
        res.set_content(j.dump(), "application/json");
    });
    // Prometheus scrape endpoint.
    http_->get("/metrics", [](const http::Request&, http::Response& res) {
        res.set_content(metrics::registry().prometheus(), "text/plain; version=0.0.4");
    });
    setup_http(*http_);
    for (auto& c : components_) c->setup_http(*http_);
    for (auto& h : hooks_.http) h(*this, *http_);
    create_web(cfg);
}

void Application::create_web(const Config& cfg) {
    if (!cfg.get_or<bool>("web.enabled", true)) return;

    web_events_ = std::make_shared<http::EventStream>(cfg.get_or<std::size_t>("web.max_clients", 8));
    web_events_->on_connect([this] {
        std::vector<http::EventStream::Message> greeting;
        if (!stopping()) greeting.push_back({"state", invoke([this] { return dashboard_->state(); })});
        greeting.push_back({"alarms", alarms_.to_json()});
        return greeting;
    });

    // Pages: the built-in dashboard under /forge/, the application's own pages
    // (embedded with forge_add_web, or served from [web].root while developing).
    http_->assets(web::builtin_assets());
    const auto root = cfg.get_or<std::string>("web.root", "");
    if (!root.empty()) {
        if (!http_->mount("/", root)) throw ConfigError("web.root '" + root + "' is not a directory");
        logger_->info("web: serving pages from {}", root);
    } else {
        http_->assets(web::registered_assets());
    }

    if (cfg.get_or<bool>("web.logs", true)) {
        auto sink = std::make_shared<WebLogSink>(web_events_, cfg.get_or<std::size_t>("web.log_history", 200));
        sink->set_level(log::parse_level(cfg.get_or<std::string>("web.log_level", "info")));
        web_log_sink_ = sink;
        log::add_sink(web_log_sink_);
    }

    const std::string api = "/forge/api";
    http_->get_json(api + "/info", [this](const http::Request&) {
        return json{{"name", info_.name},
                    {"version", info_.version},
                    {"description", info_.description},
                    {"forgecpp", version_string},
                    {"hostname", host_name()},
                    {"pid", ::getpid()},
                    {"uptime_s", std::chrono::duration_cast<std::chrono::seconds>(uptime()).count()},
                    {"logs", web_log_sink_ != nullptr},
                    {"protocol", "forge.v1"},
                    {"endpoints", endpoints()}};
    });
    http_->get_json(api + "/ui", [this](const http::Request&) { return invoke([this] { return dashboard_->schema(); }); });
    http_->get_json(api + "/state", [this](const http::Request&) { return invoke([this] { return dashboard_->state(); }); });
    http_->get_json(api + "/controls/:id", [this](const http::Request& req) {
        const auto id = req.path_params.at("id");
        return json{{"id", id}, {"value", invoke([this, &id] { return dashboard_->get(id); })}};
    });
    auto web_caller = [this](const http::Request& req) {
        return control_->identify("web", req.remote_addr, {}, req.get_header_value("Authorization"));
    };
    http_->post_json(api + "/controls/:id", [this, web_caller](const http::Request& req, const json& body) {
        const auto id = req.path_params.at("id");
        const auto caller = web_caller(req);
        const bool button = invoke([&] { return dashboard_->control(id).kind == web::Kind::button; });
        if (button) {
            control_->press(id, caller);
        } else {
            if (!(body.is_object() && body.contains("value"))) {
                throw http::HttpError(400, "request body must be {\"value\": ...}");
            }
            control_->set(id, body["value"], caller);
        }
        return json{{"id", id}, {"value", control_->get(id, caller)}};
    });
    http_->post_json(api + "/commands/:id", [this, web_caller](const http::Request& req, const json& body) {
        const auto id = req.path_params.at("id");
        json args = body.is_object() && body.contains("args") ? body["args"] : body;
        if (args.is_null()) args = json::object();
        return json{{"id", id}, {"result", control_->call(id, args, web_caller(req))}};
    });
    http_->post_json(api + "/leases/:id", [this, web_caller](const http::Request& req, const json& body) {
        const auto id = req.path_params.at("id");
        const auto s = control_->lease(id, body.is_object() ? body.value("seconds", 60u) : 60u, web_caller(req));
        return json{{"id", id}, {"seconds", s.count()}};
    });
    http_->get_json(api + "/leases", [this](const http::Request&) { return control_->leases(); });
    // Card layouts of the pages (forge.layout in forge.js): kept on the device, so
    // every browser shows the same arrangement.
    layout_file_ = cfg.get_or<std::string>("web.layout_file", "");
    if (layout_file_.empty()) layout_file_ = default_layout_file(info_.name);
    if (layout_file_ == "-") layout_file_.clear();
    if (!layout_file_.empty() && std::filesystem::exists(layout_file_)) {
        try {
            auto j = load_json(layout_file_);
            if (j.is_object()) web_layouts_ = std::move(j);
        } catch (const std::exception& e) {
            logger_->warn("ignoring {}: {}", layout_file_, e.what());
        }
    }
    auto valid_page = [](const std::string& p) {
        return !p.empty() && p.size() <= 64 && std::all_of(p.begin(), p.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_' || c == '.';
        });
    };
    http_->get_json(api + "/layout/:page", [this, valid_page](const http::Request& req) {
        const auto page = req.path_params.at("page");
        if (!valid_page(page)) throw http::HttpError(400, "invalid page name");
        std::lock_guard lock(layout_mutex_);
        return web_layouts_.contains(page) ? web_layouts_[page] : json(nullptr);
    });
    http_->put_json(api + "/layout/:page", [this, valid_page](const http::Request& req, const json& body) {
        const auto page = req.path_params.at("page");
        if (!valid_page(page)) throw http::HttpError(400, "invalid page name");
        if (!body.is_object() || !body.contains("items") || !body["items"].is_array() || body["items"].size() > 500 ||
            req.body.size() > 256 * 1024) {
            throw http::HttpError(400, "expected {\"items\": [...]} (at most 500 items)");
        }
        std::lock_guard lock(layout_mutex_);
        web_layouts_[page] = body;
        save_layouts();
        return json{{"page", page}, {"saved", !layout_file_.empty()}};
    });
    http_->del_json(api + "/layout/:page", [this, valid_page](const http::Request& req) {
        const auto page = req.path_params.at("page");
        if (!valid_page(page)) throw http::HttpError(400, "invalid page name");
        std::lock_guard lock(layout_mutex_);
        web_layouts_.erase(page);
        save_layouts();
        return json(nullptr);
    });
    http_->get_json(api + "/whoami", [web_caller](const http::Request& req) {
        const auto c = web_caller(req);
        return json{{"user", c.user}, {"role", to_string(c.role)}, {"client", c.client}};
    });
    http_->del_json(api + "/leases/:id", [this, web_caller](const http::Request& req) {
        control_->release(req.path_params.at("id"), web_caller(req));
        return json(nullptr);
    });
    http_->get_json(api + "/metrics", [](const http::Request& req) {
        return proto::to_json(metrics::registry().snapshot(req.get_param_value("prefix")));
    });
    http_->get_json(api + "/audit", [this](const http::Request& req) {
        json a = json::array();
        const auto n = req.has_param("limit") ? std::stoul(req.get_param_value("limit")) : 200;
        for (const auto& r : audit_.list(n)) a.push_back(proto::to_json(r));
        return a;
    });
    http_->get_json(api + "/health", [this](const http::Request&) { return proto::to_json(health_.snapshot()); });
    http_->get_json(api + "/streams", [this](const http::Request&) { return proto::to_json(stream_list()); });
    http_->get(api + "/trace", [](const http::Request& req, http::Response& res) {
        const unsigned seconds = req.has_param("seconds") ? std::stoul(req.get_param_value("seconds")) : 0;
        auto d = trace::dump(seconds);
        res.set_header("Content-Disposition", "attachment; filename=\"trace.json\"");
        res.set_content(std::move(d.chrome_json), "application/json");
    });
    // forge.v1 over HTTP: protobuf (application/x-protobuf) or its JSON mapping.
    http_->post(api + "/rpc", [this](const http::Request& req, http::Response& res) {
        Caller transport = control_->identify("http-rpc", req.remote_addr, {}, req.get_header_value("Authorization"));
        const auto type = req.get_header_value("Content-Type");
        if (type.find("json") != std::string::npos) {
            v1::Request r;
            try {
                proto::from_json_string(req.body, &r);
            } catch (const std::exception& e) {
                throw http::HttpError(400, e.what());
            }
            Caller c = control_->identify("http-rpc", r.client().empty() ? req.remote_addr : r.client(), r.token(),
                                          req.get_header_value("Authorization"));
            res.set_content(proto::to_json_string(control_->handle(r, c)), "application/json");
        } else {
            // A user from the Authorization header wins; otherwise Request.token decides.
            res.set_content(control_->handle_bytes(req.body, transport, req.remote_addr), "application/x-protobuf");
        }
    });
    http_->get_json(api + "/logs", [this](const http::Request&) {
        auto* sink = static_cast<WebLogSink*>(web_log_sink_.get());
        return sink ? sink->history() : json::array();
    });
    http_->get_json(api + "/alarms", [this](const http::Request&) { return alarms_.to_json(); });
    http_->post_json(api + "/alarms/ack", [this](const http::Request& req, const json&) {
        return json{{"acknowledged", alarms_.acknowledge_all("web " + req.remote_addr)}};
    });
    http_->post_json(api + "/alarms/:id/ack", [this](const http::Request& req, const json&) {
        const auto id = req.path_params.at("id");
        if (!alarms_.acknowledge(id, "web " + req.remote_addr)) throw http::HttpError(404, "unknown alarm '" + id + "'");
        return to_json(*alarms_.get(id));
    });
    alarms_.on_change([this] { alarms_dirty_ = true; });
    audit_.on_record([this](const v1::AuditRecord& r) {
        if (web_events_) web_events_->publish("audit", proto::to_json(r));
    });
    http_->events(api + "/events", web_events_);

    // "/" shows the application's own index.html when there is one, else the dashboard.
    http_->get("/", [](const http::Request&, http::Response& res) { res.set_redirect("/forge/"); });

}

void Application::push_web_state() {
    if (!dashboard_ || !web_events_ || web_events_->clients() == 0) return;
    if (alarms_dirty_.exchange(false)) web_events_->publish("alarms", alarms_.to_json());
    auto changed = dashboard_->changes();
    if (!changed.empty()) web_events_->publish("state", changed);
}

void Application::register_web_console(Console& c) {
    auto ids = [this](const Console::Args& so_far) -> std::vector<std::string> {
        if (!so_far.empty()) return {};
        return dashboard_->ids();
    };
    c.add("controls", "controls  list the web dashboard controls and their values", [this, &c](const Console::Args&) {
        const auto schema = dashboard_->schema();
        for (const auto& g : schema["groups"]) {
            c.print("[{}]", g["title"].get<std::string>());
            for (const auto& ctl : g["controls"]) {
                const auto value = ctl.contains("value") ? ctl["value"].dump() : std::string("(button)");
                c.print("  {:<20} {:<7} {}{}", ctl["id"].get<std::string>(), ctl["kind"].get<std::string>(), value,
                        ctl.contains("unit") ? " " + ctl["unit"].get<std::string>() : "");
            }
        }
    });
    c.add(
        "set", "set <id> <value>  change a dashboard control (value as JSON: 42, true, \"text\")",
        [this, &c](const Console::Args& args) {
            if (args.size() < 2) throw std::invalid_argument("usage: set <id> <value>");
            std::string text;
            for (std::size_t i = 1; i < args.size(); ++i) text += (i > 1 ? " " : "") + args[i];
            auto value = json::parse(text, nullptr, false);
            if (value.is_discarded()) value = text;  // bare words are strings
            const auto now = control_->set(args[0], value, console_caller());
            c.print("{} = {}", args[0], now.dump());
        },
        ids);
    c.add(
        "press", "press <id>  press a dashboard button",
        [this](const Console::Args& args) {
            if (args.empty()) throw std::invalid_argument("usage: press <id>");
            control_->press(args[0], console_caller());
        },
        ids);
}

void Application::drain_while(const std::function<void()>& blocking) {
    std::atomic<bool> done{false};
    std::thread helper([&] {
        try {
            blocking();
        } catch (const std::exception& e) {
            if (logger_) logger_->error("shutdown step failed: {}", e.what());
        }
        done = true;
        tasks_.push([] {});
    });
    while (!done.load()) {
        auto task = tasks_.pop_for(std::chrono::milliseconds(50));
        if (!task || !*task) continue;
        try {
            (*task)();
        } catch (const std::exception& e) {
            if (logger_) logger_->error("task failed during shutdown: {}", e.what());
        } catch (...) {
        }
    }
    helper.join();
}

void Application::create_console(const Config& cfg) {
    // The command registry always exists (telnet uses it); the local REPL
    // only runs with [console].enabled / -i.
    Console::Options o;
    o.prompt = cfg.get_or<std::string>("console.prompt", info_.name + "> ");
    o.history_file = cfg.get_or<std::string>("console.history", "");
    if (o.history_file.empty()) {
        if (const char* home = std::getenv("HOME")) {
            o.history_file = std::string(home) + "/." + info_.name + "_history";
        }
    }
    o.color = cfg.get_or<bool>("console.color", ::isatty(STDOUT_FILENO) != 0);
    o.banner = fmt::format("{} {} - type 'help' for commands, 'exit' to quit", info_.name, info_.version);

    console_ = std::make_unique<Console>(o);
    // Commands run on the main loop so they never race with application state.
    console_->set_executor([this](std::function<void()> fn) {
        if (stopping()) return;
        auto future = call(std::move(fn));
        while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
            if (stopping()) return;
        }
        future.get();
    });
    console_->on_exit([this] { stop(0); });

    register_console_builtins(*console_);
    if (dashboard_) register_web_console(*console_);
    register_framework_console(*console_);
    setup_console(*console_);
    for (auto& c : components_) c->setup_console(*console_);
    for (auto& h : hooks_.console) h(*this, *console_);
}

void Application::register_console_builtins(Console& c) {
    c.add("status", "status  show application status", [this, &c](const Console::Args&) {
        c.print("{} {} (forgecpp {})", info_.name, info_.version, version_string);
        c.print("  pid          {}", ::getpid());
        c.print("  uptime       {}", format_duration(uptime()));
        c.print("  log level    {}", spdlog::level::to_string_view(log::level()));
        c.print("  workers      {} ({} tasks queued)", pool_->get_thread_count(), pool_->get_tasks_queued());
        c.print("  timers       {}", scheduler_->pending());
        if (http_) c.print("  http         port {}", http_->port());
    });
    c.add(
        "log-level", "log-level [level]  show or change the log level",
        [this, &c](const Console::Args& args) {
            // log-level <level> | log-level <logger> <level>
            if (args.size() == 1) control_->set_log_level({}, args[0], console_caller());
            if (args.size() >= 2) control_->set_log_level(args[0], args[1], console_caller());
            c.print("log level: {}", spdlog::level::to_string_view(log::level()));
            for (const auto& [name, lvl] : log::logger_levels()) c.print("  {:<24} {}", name, lvl);
        },
        [](const Console::Args& so_far) -> std::vector<std::string> {
            if (!so_far.empty()) return {};
            return {"trace", "debug", "info", "warn", "error", "critical", "off"};
        });
    c.add("config", "config [key.path]  print the effective configuration as JSON",
          [this, &c](const Console::Args& args) {
              const auto cfg = config();
              if (args.empty()) {
                  c.print("{}", cfg.to_json().dump(2));
              } else if (auto v = cfg.get<json>(args[0])) {
                  c.print("{}", v->dump(2));
              } else {
                  c.error("no such key '" + args[0] + "'");
              }
          });
    c.add("reload", "reload  re-read the configuration file", [this](const Console::Args&) { do_reload(); });
    c.add("alarms", "alarms  list active alarms and recent history", [this, &c](const Console::Args&) {
        const auto list = alarms_.list();
        if (list.empty()) {
            c.print("no alarms");
            return;
        }
        for (const auto& a : list) {
            const auto t = Alarm::Clock::to_time_t(a.active ? a.last_at : *a.cleared_at);
            std::tm tm{};
            ::localtime_r(&t, &tm);
            c.print("  {:<8} {:<9} {:<20} {:02}:{:02}:{:02} {}{}", to_string(a.severity),
                    a.pending() ? "ACTIVE" : (a.active ? "ACKED" : "CLEARED"), a.id, tm.tm_hour, tm.tm_min,
                    tm.tm_sec, a.message, a.count > 1 ? fmt::format(" (x{})", a.count) : "");
        }
    });
    c.add(
        "ack", "ack <id|all>  acknowledge an alarm",
        [this, &c](const Console::Args& args) {
            if (args.empty()) throw std::invalid_argument("usage: ack <id|all>");
            c.print("{} alarm(s) acknowledged", control_->ack(args[0], console_caller()));
        },
        [this](const Console::Args& so_far) -> std::vector<std::string> {
            if (!so_far.empty()) return {};
            std::vector<std::string> ids{"all"};
            for (const auto& a : alarms_.list()) ids.push_back(a.id);
            return ids;
        });
}

void Application::main_loop() {
    while (!stopping()) {
        auto task = tasks_.pop_for(std::chrono::milliseconds(500));
        if (!task || !*task) continue;
        try {
            (*task)();
        } catch (const std::exception& e) {
            logger_->error("unhandled exception in main loop task: {}", e.what());
        } catch (...) {
            logger_->error("unhandled unknown exception in main loop task");
        }
    }
}

void Application::do_reload() {
    Config fresh;
    try {
        fresh = load_config();
    } catch (const std::exception& e) {
        logger_->error("reload failed, keeping the current configuration: {}", e.what());
        return;
    }
    if (!validate_config(fresh, false)) {
        logger_->error("reload failed, keeping the current configuration");
        return;
    }
    {
        std::lock_guard lock(config_mutex_);
        config_ = fresh;
    }
    if (auto level = fresh.get<std::string>("log.level")) {
        try {
            log::set_level(*level);
        } catch (const std::exception& e) {
            logger_->error("{}", e.what());
        }
    }
    try {
        on_reload(fresh);
        for (auto& c : components_) c->on_reload(fresh);
        for (auto& h : hooks_.reload) h(*this);
        logger_->info("configuration reloaded");
    } catch (const std::exception& e) {
        logger_->error("reload handler failed: {}", e.what());
    }
}

void Application::shutdown() {
    systemd::notify("STOPPING=1");
    if (logger_) logger_->info("{} stopping", info_.name);
    // Tell the hub first, so it does not mistake the end for a crash.
    if (hub_client_) {
        const bool asked = stop_signal_.load() != 0;
        hub_client_->stop(asked ? v1::Unregister::REASON_SIGNAL : v1::Unregister::REASON_EXIT, exit_code_.load());
    }

    if (telnet_) telnet_->stop();
    if (console_) console_->stop();

    // HTTP / ZeroMQ handlers may be waiting on the main loop (invoke/call): keep
    // running posted tasks while they wind down so they can finish.
    if (http_) drain_while([this] { http_->stop(); });
    if (control_server_) drain_while([this] { control_server_->stop(); });
    if (web_log_sink_) {
        log::remove_sink(web_log_sink_);
        web_log_sink_.reset();
    }

    try {
        for (auto it = components_.rbegin(); it != components_.rend(); ++it) (*it)->on_stop();
        on_stop();
        for (auto& h : hooks_.stop) h(*this);
    } catch (const std::exception& e) {
        if (logger_) logger_->error("stop handler failed: {}", e.what());
    }
    state_dirty_ = state_dirty_ || !state_file_.empty();
    save_state();
    if (log_listener_ >= 0) log::remove_listener(log_listener_);
    log_listener_ = -1;
    if (data_pub_) data_pub_->stop();
    if (events_pub_) events_pub_->stop();

    if (scheduler_) scheduler_->stop();
    if (pool_) drain_while([this] { pool_->wait(); });
    stop_signal_thread();

    if (logger_) logger_->info("{} stopped", info_.name);
    log::flush();

    telnet_.reset();
    hub_client_.reset();
    control_server_.reset();
    console_.reset();
    http_.reset();
    data_pub_.reset();
    events_pub_.reset();
    dashboard_.reset();
    web_events_.reset();
    scheduler_.reset();
    pool_.reset();
}

// ---------------------------------------------------------------------------
// Framework services

ControlService& Application::control() {
    if (!control_) throw std::logic_error("control() is available from setup_web() on");
    return *control_;
}

void Application::attach(std::unique_ptr<Component> component) {
    component->app_ = this;
    component->logger_ = log::get(component->name());
    components_.push_back(std::move(component));
}

zmq::Stream& Application::stream(const std::string& name, const std::string& type) {
    return stream(name, type, zmq::StreamOptions{});
}

zmq::Stream& Application::stream(const std::string& name, const std::string& type, const zmq::StreamOptions& o) {
    std::lock_guard lock(data_mutex_);
    if (!data_pub_) {
        // No [forge].data configured: an ephemeral port, listed in Info.endpoints.
        const auto ep = config().get_or<std::string>("forge.data", "");
        data_pub_ = std::make_unique<zmq::Publisher>(ep.empty() ? "tcp://*:0" : ep);
        logger_->info("forge: streams on {}", data_pub_->endpoint());
    }
    return data_pub_->add(name, type, o);
}

v1::StreamList Application::stream_list() const {
    v1::StreamList l;
    std::lock_guard lock(data_mutex_);
    if (data_pub_) {
        for (auto& s : data_pub_->list()) *l.add_streams() = std::move(s);
    }
    return l;
}

std::map<std::string, std::string> Application::endpoints() const {
    std::map<std::string, std::string> e;
    if (control_server_) e["control"] = control_server_->endpoint();
    if (events_pub_) e["events"] = events_pub_->endpoint();
    {
        std::lock_guard lock(data_mutex_);
        if (data_pub_) e["data"] = data_pub_->endpoint();
    }
    if (http_ && http_->port() > 0) e["http"] = fmt::format("http://{}:{}/", host_name(), http_->port());
    if (telnet_) e["telnet"] = telnet_->endpoint();
    return e;
}

std::string Application::configure_thread(const std::string& name) {
    trace::set_thread_name(name);
    const auto settings = rt::thread_settings(config(), name);
    if (!settings) return {};
    const auto err = rt::apply(*settings);
    if (err.empty()) {
        logger_->info("thread {}: {}", name, rt::describe_current_thread());
    } else {
        logger_->warn("thread {}: {}", name, err);
    }
    return err;
}

void Application::control_changed(const std::string&, bool persistent) {
    if (persistent) state_dirty_ = true;
    if (!on_main_loop()) {
        state_event_pending_ = true;
        post([this] { control_changed({}, false); });
        return;
    }
    push_web_state();
    // "state" event: the changed values (forge.v1.Values).
    if (!events_pub_ || !dashboard_) return;
    auto* s = events_pub_->find("state");
    if (!s || s->subscribers() == 0) return;
    const json current = dashboard_->state();
    v1::Values v;
    for (const auto& [id, value] : current.items()) {
        auto it = last_event_state_.find(id);
        if (it == last_event_state_.end() || *it != value) (*v.mutable_values())[id] = proto::to_value(value);
    }
    last_event_state_ = current;
    if (v.values_size() > 0) s->publish(v);
}

void Application::publish_event(const std::string& topic, const google::protobuf::Message& body) {
    if (!events_pub_) return;
    if (auto* s = events_pub_->find(topic); s && s->wanted()) s->publish(body);
}

void Application::declare_framework_config(config::Schema& s) {
    s.section("log", "Logging");
    s.string("log.level", "info", "Default level").choices({"trace", "debug", "info", "warn", "error", "critical", "off"});
    s.string("log.format", "text", "text: [log].pattern; json: one JSON object per line").choices({"text", "json"});
    s.string("log.pattern", log::Options{}.pattern, "spdlog pattern for text output");
    s.boolean("log.console", true, "Log to stderr");
    s.string("log.file", "", "Rotating log file (empty = none)");
    s.integer("log.max_size_mb", 10, "Size of one log file").range(1, 100000).unit("MB");
    s.integer("log.max_files", 3, "Rotated log files kept").range(1, 1000);
    s.boolean("log.syslog", false, "Also log to syslog");
    s.string("log.flush_level", "warn", "Flush immediately at this level");
    s.integer("log.flush_interval_s", 2, "Periodic flush").range(0, 3600).unit("s");
    s.integer("log.history", 500, "Records kept for the panel, RPC and telnet").range(10, 100000);

    s.section("http", "Web server, control panel and JSON API");
    s.boolean("http.enabled", false, "Serve HTTP");
    s.string("http.host", "0.0.0.0", "Listen address");
    s.integer("http.port", 8080, "Listen port").range(0, 65535);
    s.integer("http.threads", 8, "Worker threads").range(2, 256);
    s.open_section("http.auth", "Older single-user credentials (prefer [auth])");

    s.section("web", "Control panel");
    s.boolean("web.enabled", true, "Serve the control panel at /forge/");
    s.string("web.title", info_.name, "Title");
    s.string("web.root", "", "Serve pages from this directory instead of the embedded ones");
    s.integer("web.refresh_ms", 500, "Live update interval").range(50, 60000).unit("ms");
    s.integer("web.max_clients", 8, "Open pages at once").range(1, 1000);
    s.boolean("web.logs", true, "Show the log in the panel");
    s.string("web.layout_file", "",
             "Card layouts of the pages (default: $STATE_DIRECTORY/web-layout.json, else "
             "~/.local/state/<name>/web-layout.json; \"-\" = not stored on the device)");
    s.integer("web.log_history", 200, "Log lines shown").range(1, 100000);
    s.string("web.log_level", "info", "Lowest level shown in the panel");

    s.section("console", "Local console (-i) and telnet");
    s.boolean("console.enabled", false, "Run the console on the terminal (same as -i)");
    s.string("console.prompt", info_.name + "> ", "Prompt");
    s.string("console.history", "", "History file (default ~/.<app>_history)");
    s.boolean("console.color", true, "Colors");
    s.string("console.telnet", "", "Telnet console HOST:PORT, e.g. \"0.0.0.0:2323\" (empty = off)");
    s.integer("console.telnet_max_sessions", 4, "Telnet sessions at once").range(1, 64);
    s.integer("console.telnet_idle_s", 0, "Close idle telnet sessions after (0 = never)").range(0, 86400).unit("s");

    s.section("forge", "forge.v1 on ZeroMQ: control, events and streams");
    s.string("forge.control", "", "REP: forge.v1 requests (forgectl, panels), e.g. \"tcp://*:5557\"");
    s.string("forge.events", "", "PUB: state, alarms, log, audit, health, metrics, e.g. \"tcp://*:5559\"");
    s.string("forge.data", "", "PUB: the application's streams (empty: an ephemeral port when used)");
    s.integer("forge.metrics_interval_s", 5, "Metrics event interval").range(1, 3600).unit("s");

    s.section("auth", "Users and roles");
    s.boolean("auth.enabled", false, "Require credentials for changes (roles)");
    s.boolean("auth.anonymous", true, "Allow anonymous access with default_role (HTTP)");
    s.string("auth.default_role", "viewer", "Role without credentials").choices({"viewer", "operator", "admin"});
    s.open_section("auth.users", "[auth.users.<name>] password, token, role");

    s.section("audit", "Audit trail of control changes");
    s.integer("audit.history", 500, "Records kept in memory").range(1, 1000000);
    s.string("audit.file", "", "Also append JSON lines to this file");

    s.section("trace", "Flight recorder");
    s.boolean("trace.enabled", true, "Record spans");
    s.integer("trace.capacity", 65536, "Events kept").range(1024, 16777216);

    s.section("crash", "Crash reports");
    s.boolean("crash.enabled", true, "Write a report on fatal signals");
    s.string("crash.dir", "", "Report directory (default: $STATE_DIRECTORY or /tmp)");

    s.section("state", "Persistent controls");
    s.string("state.file", "", "Controls marked persist() are saved here (empty = not saved)");

    s.section("health", "Health and watchdog");
    s.number("health.watchdog_grace_s", 0, "Stop systemd watchdog pings after a critical component failed this long (0 = never)")
        .range(0, 86400)
        .unit("s");

    s.section("threads", "Worker pool and real-time threads");
    s.integer("threads.workers", 0, "Worker threads (0 = CPU count)").range(0, 1024);
    s.open_section("threads", "[threads.<name>] policy, priority, nice, cpus (see rt.hpp)");

    s.section("hub", "Registration with forge-hub (optional; nothing happens without a hub)");
    s.boolean("hub.register", true, "Try to register with the hub at start-up");
    s.string("hub.endpoint", hub::default_endpoint, "The hub's registration endpoint");
    s.integer("hub.retry_s", 30, "Register again every N s in the background (0 = try once)").range(0, 86400).unit("s");
    s.boolean("hub.critical", false, "Its systemd unit failing is a critical alarm on the hub");
    s.string("hub.group", "", "Group on the hub's dashboard");
    s.string("hub.title", "", "Name on the hub's dashboard (default: the application name)");

    s.section("process", "Process settings");
    s.boolean("process.mlockall", false, "Lock all memory (no page faults; needs CAP_IPC_LOCK)");
}

bool Application::validate_config(const Config& cfg, bool at_start) {
    const auto r = schema_.validate(cfg);
    for (const auto& w : r.warnings) {
        if (at_start && !logger_) {
            std::cerr << info_.name << ": warning: " << w << "\n";
        } else if (logger_) {
            logger_->warn("config: {}", w);
        }
    }
    for (const auto& e : r.errors) {
        if (logger_) {
            logger_->error("config: {}", e);
        } else {
            std::cerr << info_.name << ": config: " << e << "\n";
        }
    }
    return r.ok();
}

void Application::start_observability(const Config& cfg) {
    trace::configure(cfg.get_or<bool>("trace.enabled", true), cfg.get_or<std::size_t>("trace.capacity", 65536));
    audit_.configure(cfg.get_or<std::size_t>("audit.history", 500), cfg.get_or<std::string>("audit.file", ""));

    if (cfg.get_or<bool>("crash.enabled", true)) {
        std::string dir = cfg.get_or<std::string>("crash.dir", "");
        if (dir.empty()) {
            const char* state = std::getenv("STATE_DIRECTORY");  // systemd StateDirectory=
            dir = state ? std::string(state).substr(0, std::string(state).find(':')) : "/tmp";
        }
        if (auto report = crash::previous_report(info_.name, dir)) {
            std::ifstream in(*report);
            std::string first;
            std::getline(in, first);
            const auto kept = fmt::format("{}/{}-crash-{}.txt", dir, info_.name,
                                          std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
            std::error_code ec;
            std::filesystem::rename(*report, kept, ec);
            logger_->error("the previous run crashed ({}); report: {}", first, ec ? *report : kept);
            alarms_.raise("forge.crashed", Severity::error,
                          fmt::format("The previous run crashed ({}). Report: {}", first, ec ? *report : kept));
        }
        crash::install(info_.name, dir);
    }
    if (cfg.get_or<bool>("process.mlockall", false)) {
        if (auto err = rt::lock_memory(); err.empty()) {
            logger_->info("memory locked (mlockall)");
        } else {
            logger_->warn("{}", err);
        }
    }

    // Process metrics, computed when read.
    auto& m = metrics::registry();
    m.gauge_fn("process.uptime_seconds", [this] { return std::chrono::duration<double>(uptime()).count(); }, "Uptime", "s");
    m.gauge_fn("process.rss_bytes", [] {
        long pages = 0, rss = 0;
        if (FILE* f = std::fopen("/proc/self/statm", "r")) {
            if (std::fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0;
            std::fclose(f);
        }
        return static_cast<double>(rss) * static_cast<double>(::sysconf(_SC_PAGESIZE));
    }, "Resident memory", "bytes");
    m.gauge_fn("process.cpu_seconds", [] {
        rusage u{};
        ::getrusage(RUSAGE_SELF, &u);
        return static_cast<double>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) +
               static_cast<double>(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
    }, "CPU time (user + system)", "s");
    m.gauge_fn("process.threads", [] {
        std::error_code ec;
        std::size_t n = 0;
        for (auto it = std::filesystem::directory_iterator("/proc/self/task", ec);
             !ec && it != std::filesystem::directory_iterator(); ++it) {
            ++n;
        }
        return static_cast<double>(n);
    }, "Threads");
    m.gauge_fn("forge.pool.queued", [this] { return pool_ ? static_cast<double>(pool_->get_tasks_queued()) : 0.0; },
               "Tasks waiting for a worker thread");
    m.gauge_fn("forge.scheduler.timers", [this] { return scheduler_ ? static_cast<double>(scheduler_->pending()) : 0.0; },
               "Pending timers");
    m.gauge_fn("forge.alarms.active", [this] { return static_cast<double>(alarms_.active_count()); }, "Active alarms");
    m.gauge_fn("forge.health.state", [this] { return static_cast<double>(health_.overall()); },
               "Overall health: 1 ok, 2 degraded, 3 failed");
}

void Application::start_messaging(const Config& cfg) {
    const auto events = cfg.get_or<std::string>("forge.events", "");
    if (!events.empty()) {
        zmq::SocketOptions so;
        so.send_hwm = 1000;
        events_pub_ = std::make_unique<zmq::Publisher>(events, so);
        events_pub_->add("state", "forge.v1.Values");
        events_pub_->add("alarms", "forge.v1.AlarmList");
        events_pub_->add("log", "forge.v1.LogRecord");
        events_pub_->add("audit", "forge.v1.AuditRecord");
        events_pub_->add("health", "forge.v1.Health");
        events_pub_->add("metrics", "forge.v1.MetricsSnapshot");
        alarms_.on_change([this] {
            if (!events_pub_) return;
            auto* s = events_pub_->find("alarms");
            if (!s || s->subscribers() == 0) return;
            v1::Request r;
            r.mutable_alarms();
            Caller internal;
            const auto reply = control_->handle(r, internal);
            s->publish(reply.alarms());
        });
        audit_.on_record([this](const v1::AuditRecord& r) { publish_event("audit", r); });
        health_.on_change([this](const v1::Health& h) { publish_event("health", h); });
        log_listener_ = log::add_listener([this](const v1::LogRecord& r) { publish_event("log", r); });
        const auto interval = std::chrono::seconds(std::max(cfg.get_or<int>("forge.metrics_interval_s", 5), 1));
        every(interval, [this] { publish_event("metrics", metrics::registry().snapshot()); });
        logger_->info("forge: events on {}", events_pub_->endpoint());
    }
    const auto data = cfg.get_or<std::string>("forge.data", "");
    if (!data.empty()) {
        std::lock_guard lock(data_mutex_);
        data_pub_ = std::make_unique<zmq::Publisher>(data);
        logger_->info("forge: streams on {}", data_pub_->endpoint());
    }
    const auto control = cfg.get_or<std::string>("forge.control", "");
    if (!control.empty()) {
        control_server_ = zmq::serve_control(*control_, control);
        logger_->info("forge: control on {}", control_server_->endpoint());
    }
}

void Application::start_telnet(const Config& cfg) {
    const auto listen = cfg.get_or<std::string>("console.telnet", "");
    if (listen.empty() || !console_) return;
    Telnet::Options o;
    o.listen = listen;
    o.max_sessions = cfg.get_or<std::size_t>("console.telnet_max_sessions", 4);
    o.idle_s = cfg.get_or<unsigned>("console.telnet_idle_s", 0);
    o.prompt = cfg.get_or<std::string>("console.prompt", info_.name + "> ");
    o.banner = fmt::format("{} {} on {} (forgecpp {})", info_.name, info_.version, host_name(), version_string);
    telnet_ = std::make_unique<Telnet>(*console_, auth_, o);
    telnet_->start();
    logger_->info("console: telnet on {}", telnet_->endpoint());
}

void Application::restore_state() {
    state_file_ = config().get_or<std::string>("state.file", "");
    if (state_file_.empty() || !dashboard_) return;
    std::error_code ec;
    if (!std::filesystem::exists(state_file_, ec)) return;
    try {
        const auto saved = load_json(state_file_);
        const auto ids = dashboard_->persistent_ids();
        std::size_t n = 0;
        for (const auto& id : ids) {
            if (!saved.contains(id)) continue;
            try {
                dashboard_->set(id, saved[id]);
                ++n;
            } catch (const std::exception& e) {
                logger_->warn("state: cannot restore '{}': {}", id, e.what());
            }
        }
        logger_->info("state: restored {} control(s) from {}", n, state_file_);
    } catch (const std::exception& e) {
        logger_->warn("state: ignoring {}: {}", state_file_, e.what());
    }
}

void Application::save_state() {
    if (state_file_.empty() || !dashboard_ || !state_dirty_.exchange(false)) return;
    json j = json::object();
    for (const auto& id : dashboard_->persistent_ids()) j[id] = dashboard_->get(id);
    try {
        save_json(state_file_, j);
    } catch (const std::exception& e) {
        logger_->warn("state: cannot save {}: {}", state_file_, e.what());
    }
}

void Application::watchdog_tick() {
    const double grace = config().get_or<double>("health.watchdog_grace_s", 0);
    if (grace > 0) {
        if (auto failed = health_.critical_failed_for(); failed && *failed > grace) {
            if (!watchdog_paused_) {
                logger_->critical("a critical component failed {:.0f} s ago: stopping watchdog pings (systemd restarts us)",
                                  *failed);
            }
            watchdog_paused_ = true;
            return;
        }
    }
    watchdog_paused_ = false;
    systemd::notify("WATCHDOG=1");
}

void Application::register_framework_console(Console& c) {
    c.add("metrics", "metrics [prefix]  show metrics", [&c](const Console::Args& args) {
        const auto snap = metrics::registry().snapshot(args.empty() ? "" : args[0]);
        for (const auto& m : snap.metrics()) {
            if (m.type() == v1::Metric::TYPE_HISTOGRAM) {
                const double mean = m.count() ? m.sum() / static_cast<double>(m.count()) : 0;
                c.print("  {:<36} count {} mean {:.6g}{}", m.name(), m.count(), mean,
                        m.unit().empty() ? "" : " " + m.unit());
            } else {
                c.print("  {:<36} {:.6g}{}", m.name(), m.value(), m.unit().empty() ? "" : " " + m.unit());
            }
        }
    });
    c.add("trace", "trace [seconds] [file]  save the flight recorder (Chrome JSON, open in ui.perfetto.dev)",
          [&c](const Console::Args& args) {
              const unsigned seconds = args.empty() ? 0 : static_cast<unsigned>(std::stoul(args[0]));
              const std::string file = args.size() > 1 ? args[1] : "/tmp/forge-trace.json";
              auto d = trace::dump(seconds);
              std::ofstream(file) << d.chrome_json;
              c.print("{} events written to {} ({} overwritten)", d.events, file, d.dropped);
          });
    c.add("audit", "audit [n]  recent control changes", [this, &c](const Console::Args& args) {
        const std::size_t n = args.empty() ? 20 : std::stoul(args[0]);
        for (const auto& r : audit_.list(n)) {
            const auto t = static_cast<std::time_t>(r.time_unix_ns() / 1000000000);
            std::tm tm{};
            ::localtime_r(&t, &tm);
            c.print("  {:02}:{:02}:{:02} {:<26} {:<7} {:<20} {}{}", tm.tm_hour, tm.tm_min, tm.tm_sec,
                    (r.user().empty() ? r.channel() : r.user() + "@" + r.channel()) + " " + r.client(), r.action(),
                    r.target(), r.has_new_value() ? proto::to_json(r.new_value()).dump() : "",
                    r.ok() ? "" : " REFUSED: " + r.error());
        }
    });
    c.add("health", "health  component health", [this, &c](const Console::Args&) {
        const auto h = health_.snapshot();
        c.print("overall: {}", to_string(static_cast<HealthState>(h.state())));
        for (const auto& comp : h.components()) {
            c.print("  {:<20} {:<9} {}{}", comp.name(), to_string(static_cast<HealthState>(comp.state())),
                    comp.message(), comp.critical() ? "" : " (non-critical)");
        }
    });
    c.add("streams", "streams  published streams and endpoints", [this, &c](const Console::Args&) {
        for (const auto& [k, v] : endpoints()) c.print("  {:<8} {}", k, v);
        const auto list = stream_list();
        for (const auto& s : list.streams()) {
            c.print("  stream {:<16} {:<22} {} subscriber(s), {} sent, {} dropped", s.name(), s.type(), s.subscribers(),
                    s.sent(), s.dropped());
        }
    });
    c.add(
        "call", "call <command> [name=value ...]  run a command",
        [this, &c](const Console::Args& args) {
            if (args.empty()) throw std::invalid_argument("usage: call <command> [name=value ...]");
            json a = json::object();
            for (std::size_t i = 1; i < args.size(); ++i) {
                const auto eq = args[i].find('=');
                if (eq == std::string::npos) throw std::invalid_argument("arguments are name=value");
                auto v = json::parse(args[i].substr(eq + 1), nullptr, false);
                a[args[i].substr(0, eq)] = v.is_discarded() ? json(args[i].substr(eq + 1)) : v;
            }
            const auto result = control_->call(args[0], a, console_caller());
            if (!result.is_null()) c.print("{}", result.dump(2));
        },
        [this](const Console::Args& so_far) -> std::vector<std::string> {
            if (!so_far.empty()) return {};
            return dashboard_ ? dashboard_->command_ids() : std::vector<std::string>{};
        });
    c.add("lease", "lease <id|group:title> [seconds]  hold a control so nobody else changes it",
          [this, &c](const Console::Args& args) {
              if (args.empty()) throw std::invalid_argument("usage: lease <id> [seconds]");
              const auto s = control_->lease(args[0], args.size() > 1 ? std::stoul(args[1]) : 60, console_caller());
              c.print("{} leased for {} s", args[0], s.count());
          });
    c.add("release", "release <id>  give a lease back", [this](const Console::Args& args) {
        if (args.empty()) throw std::invalid_argument("usage: release <id>");
        control_->release(args[0], console_caller());
    });
}

// ---------------------------------------------------------------------------
// Component

Component::Component(std::string name, bool critical) : name_(std::move(name)), critical_(critical) {}
Component::~Component() = default;
void Component::setup_config(config::Schema&) {}
void Component::setup_web(web::Dashboard&) {}
void Component::setup_console(Console&) {}
void Component::setup_http(http::Server&) {}
std::string Application::default_layout_file(const std::string& name) {
    // systemd's StateDirectory= (the units forge_add_app generates have one).
    if (const char* sd = std::getenv("STATE_DIRECTORY"); sd && *sd) {
        std::string dir(sd);
        return dir.substr(0, dir.find(':')) + "/web-layout.json";
    }
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg) return std::string(xdg) + "/" + name + "/web-layout.json";
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.local/state/" + name + "/web-layout.json";
    }
    return {};
}

void Application::save_layouts() {
    if (layout_file_.empty()) return;
    try {
        const std::filesystem::path p(layout_file_);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
        save_json(p, web_layouts_, 1);
    } catch (const std::exception& e) {
        logger_->warn("cannot save {}: {}", layout_file_, e.what());
    }
}

void Application::start_hub_client(const Config& cfg) {
    if (!hub_registration_ || !cfg.get_or<bool>("hub.register", true)) return;
    const auto endpoint = cfg.get_or<std::string>("hub.endpoint", hub::default_endpoint);
    if (endpoint.empty()) return;
    const auto retry = std::chrono::seconds(std::max(cfg.get_or<int>("hub.retry_s", 30), 0));
    hub_client_ = std::make_unique<hub::Client>(endpoint, retry, [this] {
        const auto c = config();
        v1::AppRegistration r;
        r.set_name(info_.name);
        r.set_version(info_.version);
        r.set_description(info_.description);
        r.set_pid(static_cast<std::uint32_t>(::getpid()));
        r.set_http_port(http_ ? static_cast<std::uint32_t>(std::max(http_->port(), 0)) : 0);
        for (const auto& [k, v] : endpoints()) {
            if (k != "http") (*r.mutable_endpoints())[k] = v;
        }
        r.set_critical(c.get_or<bool>("hub.critical", false));
        r.set_group(c.get_or<std::string>("hub.group", ""));
        r.set_title(c.get_or<std::string>("hub.title", ""));
        r.set_unit(hub::unit_of(::getpid()));
        r.set_forgecpp(version_string);
        return r;
    });
    hub_client_->start();
}

void Component::on_start() {}
void Component::on_stop() {}
void Component::on_reload(const Config&) {}

Application& Component::app() {
    if (!app_) throw std::logic_error("component '" + name_ + "' is not attached to an application");
    return *app_;
}

Logger Component::logger() const { return logger_ ? logger_ : log::get(); }

void Component::set_health(HealthState state, const std::string& message) {
    if (app_) app_->health().set(name_, state, message, critical_);
}

}  // namespace forge
