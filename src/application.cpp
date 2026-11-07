#include "forge/application.hpp"

#include "forge/console.hpp"
#include "forge/http.hpp"
#include "forge/systemd.hpp"
#include "forge/version.hpp"
#include "forge/web.hpp"

#include <CLI/CLI.hpp>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
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

    cli.option_defaults()->group("Options");
}

Application::~Application() {
    // Normally torn down by run(); make sure nothing outlives the object.
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
    if (task) tasks_.push(std::move(task));
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

        // The control registry exists even without HTTP: the console and
        // other channels (e.g. forge::zmq::serve_application) use it too.
        dashboard_ = std::make_unique<web::Dashboard>(cfg.get_or<std::string>("web.title", info_.name));
        setup_web(*dashboard_);
        for (auto& h : hooks_.web) h(*this, *dashboard_);

        create_http(cfg);
        create_console(cfg);

        on_start();
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
        if (console_) console_->start();

        if (auto wd = systemd::watchdog_interval()) {
            every(*wd / 2, [] { systemd::notify("WATCHDOG=1"); }, true);
            logger_->debug("systemd watchdog enabled ({} ms)",
                           std::chrono::duration_cast<std::chrono::milliseconds>(*wd).count());
        }
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

    http::Auth auth;
    auth.user = cfg.get_or<std::string>("http.auth.user", "");
    auth.password = cfg.get_or<std::string>("http.auth.password", "");
    auth.token = cfg.get_or<std::string>("http.auth.token", "");
    if (auth.enabled()) {
        http_->auth(auth);
    } else if (!is_loopback(cfg.get_or<std::string>("http.host", "0.0.0.0"))) {
        logger_->warn("http: no [http.auth] configured - anyone who can reach port {} can use the API",
                      cfg.get_or<int>("http.port", 8080));
    }

    http_->get_json("/health", [this](const http::Request&) {
        return json{{"status", stopping() ? "stopping" : "ok"},
                    {"name", info_.name},
                    {"version", info_.version},
                    {"uptime_s", std::chrono::duration_cast<std::chrono::seconds>(uptime()).count()},
                    {"alarms", alarms_.summary()}};
    });
    setup_http(*http_);
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
                    {"logs", web_log_sink_ != nullptr}};
    });
    http_->get_json(api + "/ui", [this](const http::Request&) { return invoke([this] { return dashboard_->schema(); }); });
    http_->get_json(api + "/state", [this](const http::Request&) { return invoke([this] { return dashboard_->state(); }); });
    http_->get_json(api + "/controls/:id", [this](const http::Request& req) {
        const auto id = req.path_params.at("id");
        return json{{"id", id}, {"value", invoke([this, &id] { return dashboard_->get(id); })}};
    });
    http_->post_json(api + "/controls/:id", [this](const http::Request& req, const json& body) {
        const auto id = req.path_params.at("id");
        const bool has_value = body.is_object() && body.contains("value");
        return invoke([&]() -> json {
            const auto& control = dashboard_->control(id);
            if (control.kind == web::Kind::button) {
                dashboard_->press(id);
                logger_->info("web: {} pressed '{}'", req.remote_addr, id);
            } else {
                if (!has_value) throw http::HttpError(400, "request body must be {\"value\": ...}");
                dashboard_->set(id, body["value"]);
                logger_->info("web: {} set '{}' = {}", req.remote_addr, id, body["value"].dump());
            }
            push_web_state();  // let every open page see the change right away
            return json{{"id", id}, {"value", dashboard_->get(id)}};
        });
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
            dashboard_->set(args[0], value);
            logger_->info("console: set '{}' = {}", args[0], value.dump());
            push_web_state();
            c.print("{} = {}", args[0], dashboard_->get(args[0]).dump());
        },
        ids);
    c.add(
        "press", "press <id>  press a dashboard button",
        [this](const Console::Args& args) {
            if (args.empty()) throw std::invalid_argument("usage: press <id>");
            dashboard_->press(args[0]);
            logger_->info("console: pressed '{}'", args[0]);
            push_web_state();
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
    if (!cfg.get_or<bool>("console.enabled", false)) return;

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
    setup_console(*console_);
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
        [&c](const Console::Args& args) {
            if (!args.empty()) log::set_level(args[0]);
            c.print("log level: {}", spdlog::level::to_string_view(log::level()));
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
            if (args[0] == "all") {
                c.print("{} alarm(s) acknowledged", alarms_.acknowledge_all("console"));
            } else if (!alarms_.acknowledge(args[0], "console")) {
                throw std::invalid_argument("unknown alarm '" + args[0] + "'");
            }
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
        for (auto& h : hooks_.reload) h(*this);
        logger_->info("configuration reloaded");
    } catch (const std::exception& e) {
        logger_->error("reload handler failed: {}", e.what());
    }
}

void Application::shutdown() {
    systemd::notify("STOPPING=1");
    if (logger_) logger_->info("{} stopping", info_.name);

    if (console_) console_->stop();

    // HTTP handlers may be waiting on the main loop (invoke/call): keep running
    // posted tasks while the server winds down so they can finish.
    if (http_) drain_while([this] { http_->stop(); });
    if (web_log_sink_) {
        log::remove_sink(web_log_sink_);
        web_log_sink_.reset();
    }

    try {
        on_stop();
        for (auto& h : hooks_.stop) h(*this);
    } catch (const std::exception& e) {
        if (logger_) logger_->error("stop handler failed: {}", e.what());
    }

    if (scheduler_) scheduler_->stop();
    if (pool_) drain_while([this] { pool_->wait(); });
    stop_signal_thread();

    if (logger_) logger_->info("{} stopped", info_.name);
    log::flush();

    console_.reset();
    http_.reset();
    dashboard_.reset();
    web_events_.reset();
    scheduler_.reset();
    pool_.reset();
}

}  // namespace forge
