#include "forge/application.hpp"

#include "app/services.hpp"

#include "forge/console.hpp"
#include "forge/control.hpp"
#include "forge/crash.hpp"
#include "forge/http.hpp"
#include "forge/rt.hpp"
#include "forge/systemd.hpp"
#include "forge/hub.hpp"
#include "forge/trace.hpp"
#include "forge/version.hpp"
#include "forge/web.hpp"
#include "forge/zmq.hpp"

#include <fstream>
#include <sys/resource.h>

#include <CLI/CLI.hpp>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    } catch (const Error& e) {  // forge::Error and http::HttpError
        kind = kHttp;
        status = e.status();
        message = e.what();
        body = e.details();
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
        // an http::HttpError is a forge::Error too: either catch works
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

}  // namespace

Application::Application(AppInfo info, const std::string& installed_config)
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
                         std::format("{} {} (forgecpp {})", info_.name, info_.version, version_string));
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
    cli.add_option("--instance", instance_,
                   "Instance name when the application runs several times (default: the instance of its "
                   "systemd template unit <name>@<instance>, or $FORGE_INSTANCE)")
        ->type_name("NAME");
    cli.add_flag("--print-config", print_config_, "Print the effective configuration as JSON and exit");
    cli.add_flag("--help-config", help_config_, "Describe every configuration key and exit");
    cli.add_flag("--check-config", check_config_, "Validate the configuration and exit (status 0 = valid)");

    cli.option_defaults()->group("Options");

    state_ = std::make_unique<detail::StateStore>();
    watchdog_ = std::make_unique<detail::Watchdog>();
    hub_ = std::make_unique<detail::HubRegistration>(*this);
    messaging_ = std::make_unique<detail::Messaging>(*this);
    console_ = std::make_unique<detail::RemoteConsole>(*this);
    http_ = std::make_unique<detail::HttpFrontend>(*this);
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

void Application::background_failed(const char* what) {
    if (logger_) logger_->error("background task failed: {}", what);
}

void Application::loop_busy() const {
    static auto& timeouts = metrics::registry().counter(
        "forge.control.timeouts", "Requests that gave up waiting for a busy main loop");
    timeouts.inc();
    const auto lag = loop_age_ms();
    throw Error(503, std::format("main loop busy (last heartbeat {} ms ago)", lag),
                          json{{"error", "main loop busy"}, {"loop_age_ms", lag},
                               {"timeout_ms", request_timeout_ms_.load()}});
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
    static auto& slow = metrics::registry().counter(
        "forge.loop.slow_tasks", "Main loop tasks that took longer than [forge].slow_task_ms");
    tasks_.push([this, t = std::move(task), queued = std::chrono::steady_clock::now()] {
        const auto start = std::chrono::steady_clock::now();
        lag.observe(std::chrono::duration<double>(start - queued).count());
        FORGE_TRACE_SCOPE("loop.task", "forge");
        t();
        const auto took = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
        busy.inc(static_cast<std::uint64_t>(took.count()));
        // Everything else waits while a task runs: point at it (at most every 10 s).
        const int limit_ms = slow_task_ms_.load(std::memory_order_relaxed);
        if (limit_ms > 0 && took > std::chrono::milliseconds(limit_ms)) {
            slow.inc();
            static std::atomic<std::int64_t> last_warning{0};
            const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(start.time_since_epoch()).count();
            if (now_s - last_warning.exchange(now_s) >= 10 && logger_) {
                logger_->warn("a main loop task took {} ms (> [forge].slow_task_ms = {}): everything else waited; "
                              "move heavy work off the loop with background() (forge.loop.slow_tasks, trace \"loop.task\")",
                              took.count() / 1000, limit_ms);
            }
        }
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
    if (http_) http_->publish(event, data);
}

http::Server* Application::http() { return http_ ? http_->server() : nullptr; }
Console* Application::console() { return console_ ? console_->console() : nullptr; }

Scheduler::Clock::time_point Application::now() const {
    return scheduler_ ? scheduler_->now() : Scheduler::Clock::now();
}

std::size_t Application::advance_time(Duration d) {
    if (!scheduler_ || !scheduler_->manual()) {
        throw std::logic_error("advance_time() needs use_manual_clock() before run()");
    }
    if (on_main_loop()) throw std::logic_error("advance_time() waits for the main loop: call it from the test");
    const auto target = scheduler_->now() + d;
    std::size_t ran = 0;
    // One due time at a time: what a task schedules (after(), every()) is seen
    // by the next step, and every task reads now() == the time it was due.
    while (auto next = scheduler_->next_due()) {
        if (*next > target) break;
        ran += scheduler_->advance(*next - scheduler_->now());
        invoke([] {});  // the posted tasks have run
    }
    scheduler_->advance(target - scheduler_->now());
    invoke([] {});
    return ran;
}

std::int64_t Application::loop_age_ms() const {
    if (manual_clock_) return 0;  // the heartbeat timer only runs when a test advances time
    const auto tick = loop_tick_ms_.load();
    if (tick <= 0) return 0;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    return std::max<std::int64_t>(now - tick, 0);
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
        schema_.apply_defaults(cfg);  // the schema is the one place defaults are written
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
    zmq::set_source(info_.name);
    metrics::registry().set_labels({{"app", info_.name}});

    try {
        setup_logging(cfg);
    } catch (const std::exception& e) {
        std::cerr << info_.name << ": invalid logging configuration: " << e.what() << "\n";
        return 1;
    }

    int rc = 0;
    try {
        const auto workers = cfg.value<int>("threads.workers");
        pool_ = std::make_unique<ThreadPool>(workers > 0 ? static_cast<std::size_t>(workers)
                                                         : std::thread::hardware_concurrency());
        auto to_main_loop = [this](Scheduler::Task t) { post(std::move(t)); };
        scheduler_ = manual_clock_ ? std::make_unique<Scheduler>(Scheduler::manual_clock, to_main_loop)
                                   : std::make_unique<Scheduler>(to_main_loop);
        start_signal_thread();

        logger_->info("{} {} starting (forgecpp {}, pid {}, {} worker threads)", id(), info_.version,
                      version_string, ::getpid(), pool_->get_thread_count());
        if (!is_standard_name(info_.name)) {
            logger_->warn("'{}' does not follow the daemon naming standard: lowercase words separated by '-' "
                          "(e.g. radio-controller); the name is used for the binary, config file, systemd unit "
                          "and forge-hub entry",
                          info_.name);
        }
        if (!cfg.source().empty()) logger_->info("configuration: {}", cfg.source().string());
        trace::set_thread_name("main");

        auth_.load(cfg);
        request_timeout_ms_ = cfg.value<int>("forge.request_timeout_ms");
        slow_task_ms_ = cfg.value<int>("forge.slow_task_ms");
        start_observability(cfg);
        control_ = std::make_unique<ControlService>(*this);
        detail::register_builtin_commands(*this);

        // The control registry exists even without HTTP: the console and
        // other channels (ZeroMQ, forgectl, telnet) use it too.
        dashboard_ = std::make_unique<web::Dashboard>(cfg.value<std::string>("web.title"));
        setup_web(*dashboard_);
        for (auto& c : components_) c->setup_web(*dashboard_);
        for (auto& h : hooks_.web) h(*this, *dashboard_);
        dashboard_->bind_to_thread(main_thread_.load());  // its values belong to the main loop
        state_->restore(*dashboard_, cfg.value<std::string>("state.file"), logger_);

        commands_closed_ = true;
        http_->create(cfg);
        console_->create(cfg);
        messaging_->start(cfg);
        console_->start_telnet(cfg);

        on_start();
        for (auto& c : components_) c->on_start();
        for (auto& h : hooks_.start) h(*this);

        http_->start(cfg);
        if (cfg.value<bool>("console.enabled")) console_->start_repl();

        if (auto wd = systemd::watchdog_interval()) {
            every(*wd / 2, [this] {
                if (watchdog_->tick(health_, config().value<double>("health.watchdog_grace_s"), logger_)) {
                    systemd::notify("WATCHDOG=1");
                }
            }, true);
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
            state_->save(*dashboard_, logger_);
            if (state_event_pending_.exchange(false)) control_changed({}, false);
        });
        if (hub_registration_) hub_->start(cfg);
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
    if (instance_.empty()) {
        if (const char* env = std::getenv("FORGE_INSTANCE"); env && *env) {
            instance_ = env;
        } else {
            instance_ = hub::instance_of(hub::unit_of(::getpid()));  // radio-controller@rx2.service
        }
    }
    if (instance_.size() > 32 ||
        instance_.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") != std::string::npos) {
        std::cerr << info_.name << ": invalid instance name '" << instance_ << "'\n";
        return 2;
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
    o.format = cfg.value<std::string>("log.format");
    o.history = cfg.value<std::size_t>("log.history");
    o.name = info_.name;
    o.level = cfg.value<std::string>("log.level");
    o.pattern = cfg.value<std::string>("log.pattern");
    o.console = cfg.value<bool>("log.console");
    o.color = cfg.value<std::string>("log.color");
    o.file = cfg.value<std::string>("log.file");
    o.max_size_mb = cfg.value<std::size_t>("log.max_size_mb");
    o.max_files = cfg.value<std::size_t>("log.max_files");
    o.syslog = cfg.value<bool>("log.syslog");
    o.flush_level = cfg.value<std::string>("log.flush_level");
    o.flush_interval_s = cfg.value<int>("log.flush_interval_s");
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
            if (logger_) logger_->error("task failed during shutdown: unknown exception");
        }
    }
    helper.join();
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
        schema_.apply_defaults(fresh);
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
    request_timeout_ms_ = fresh.value<int>("forge.request_timeout_ms");
    slow_task_ms_ = fresh.value<int>("forge.slow_task_ms");
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
    const bool asked = stop_signal_.load() != 0;
    hub_->stop(asked ? forge_hub_unregister_reason_REASON_SIGNAL : forge_hub_unregister_reason_REASON_EXIT,
               exit_code_.load());

    console_->stop();

    // HTTP / ZeroMQ handlers may be waiting on the main loop (invoke/call): keep
    // running posted tasks while they wind down so they can finish.
    drain_while([this] { http_->stop(); });
    drain_while([this] { messaging_->stop_control(); });

    try {
        for (auto it = components_.rbegin(); it != components_.rend(); ++it) (*it)->on_stop();
        on_stop();
        for (auto& h : hooks_.stop) h(*this);
    } catch (const std::exception& e) {
        if (logger_) logger_->error("stop handler failed: {}", e.what());
    }
    if (dashboard_) state_->save(*dashboard_, logger_, true);
    messaging_->stop();

    if (scheduler_) scheduler_->stop();
    if (pool_) drain_while([this] { pool_->wait(); });
    stop_signal_thread();

    if (logger_) logger_->info("{} stopped", info_.name);
    log::flush();

    hub_->reset();
    console_->reset();
    http_->reset();
    messaging_->reset();
    dashboard_.reset();
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

zmq::Stream& Application::stream(const std::string& name, const pb::Type& type) {
    return messaging_->stream(name, type, zmq::StreamOptions{});
}

zmq::Stream& Application::stream(const std::string& name, const pb::Type& type, const zmq::StreamOptions& o) {
    return messaging_->stream(name, type, o);
}

std::vector<zmq::StreamInfo> Application::stream_list() const { return messaging_->stream_list(); }

std::map<std::string, std::string> Application::endpoints() const {
    std::map<std::string, std::string> e;
    messaging_->add_endpoints(e);
    http_->add_endpoints(e);
    console_->add_endpoints(e);
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
    if (persistent) state_->mark_dirty();
    if (!on_main_loop()) {
        state_event_pending_ = true;
        post([this] { control_changed({}, false); });
        return;
    }
    http_->push_state();
    // "state" events: the values that changed.
    if (dashboard_ && messaging_->wants_state()) messaging_->publish_state(dashboard_->state());
}

void Application::declare_framework_config(config::Schema& s) {
    s.section("log", "Logging");
    s.string("log.level", "info", "Default level").choices({"trace", "debug", "info", "warn", "error", "critical", "off"});
    s.string("log.format", "text", "text: [log].pattern; json: one JSON object per line").choices({"text", "json"});
    s.string("log.pattern", log::Options{}.pattern,
             "Text lines: {time} {level} {logger} {message} {thread} {file} {line}");
    s.boolean("log.console", true, "Log to stderr");
    s.string("log.color", "auto", "Level colours on stderr: auto = on a terminal, not with NO_COLOR or journald")
        .choices({"auto", "always", "never"});
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
    s.boolean("console.color", true, "Colors (default: on a terminal)").computed();
    s.string("console.telnet", "", "Telnet console HOST:PORT, e.g. \"0.0.0.0:2323\" (empty = off)");
    s.integer("console.telnet_max_sessions", 4, "Telnet sessions at once").range(1, 64);
    s.integer("console.telnet_idle_s", 0, "Close idle telnet sessions after (0 = never)").range(0, 86400).unit("s");

    s.section("forge", "ZeroMQ: commands, events and streams");
    s.string("forge.control", "", "Commands (c2 frames: forgectl, other programs), e.g. \"tcp://*:5557\"");
    s.string("forge.events", "", "PUB: state, alarms, log, audit, health, metrics, e.g. \"tcp://*:5559\"");
    s.string("forge.data", "", "PUB: the application's streams (empty: an ephemeral port when used)");
    s.integer("forge.metrics_interval_s", 5, "Metrics event interval").range(1, 3600).unit("s");
    s.integer("forge.slow_task_ms", 200,
              "Warn when one main loop task (timer, control, command) runs longer than this (0 = never)")
        .range(0, 60000)
        .unit("ms");
    s.integer("forge.control_workers", 2, "Commands on [forge].control handled at the same time").range(1, 16);
    s.integer("forge.request_timeout_ms", 2000,
              "How long a request (a command, HTTP) waits for a busy main loop before it is refused")
        .range(10, 600000)
        .unit("ms");

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
    trace::configure(cfg.value<bool>("trace.enabled"), cfg.value<std::size_t>("trace.capacity"));
    audit_.configure(cfg.value<std::size_t>("audit.history"), cfg.value<std::string>("audit.file"));

    if (cfg.value<bool>("crash.enabled")) {
        std::string dir = cfg.value<std::string>("crash.dir");
        if (dir.empty()) {
            const char* state = std::getenv("STATE_DIRECTORY");  // systemd StateDirectory=
            dir = state ? std::string(state).substr(0, std::string(state).find(':')) : "/tmp";
        }
        if (auto report = crash::previous_report(info_.name, dir)) {
            std::ifstream in(*report);
            std::string first;
            std::getline(in, first);
            const auto kept = std::format("{}/{}-crash-{}.txt", dir, info_.name,
                                          std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
            std::error_code ec;
            std::filesystem::rename(*report, kept, ec);
            logger_->error("the previous run crashed ({}); report: {}", first, ec ? *report : kept);
            alarms_.raise("forge.crashed", Severity::error,
                          std::format("The previous run crashed ({}). Report: {}", first, ec ? *report : kept));
        }
        crash::install(info_.name, dir);
    }
    if (cfg.value<bool>("process.mlockall")) {
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

// ---------------------------------------------------------------------------
// Component

Component::Component(std::string name, bool critical) : name_(std::move(name)), critical_(critical) {}
Component::~Component() = default;
void Component::setup_config(config::Schema&) {}
void Component::setup_web(web::Dashboard&) {}
void Component::setup_console(Console&) {}
void Component::setup_http(http::Server&) {}
bool is_standard_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || !std::islower(static_cast<unsigned char>(name.front())) || name.back() == '-') {
        return false;
    }
    char prev = 0;
    for (const char c : name) {
        const bool ok = std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '-';
        if (!ok || (c == '-' && prev == '-')) return false;
        prev = c;
    }
    return true;
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
