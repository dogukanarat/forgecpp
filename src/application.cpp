// forge::Application: the public methods forward to Application::Impl
// (src/app/impl.hpp); the lifecycle is in Impl::run() and reads top to bottom:
// prepare, start, the main loop, shutdown.
#include "forge/application.hpp"

#include "app/impl.hpp"

#include "forge/crash.hpp"
#include "forge/http.hpp"
#include "forge/hub.hpp"
#include "forge/rt.hpp"
#include "forge/systemd.hpp"
#include "forge/trace.hpp"
#include "forge/version.hpp"
#include "forge/zmq.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>

namespace forge {

using detail::ComponentAccess;

// ---------------------------------------------------------------------------
// Construction

Application::Impl::Impl(Application& a, AppInfo i, const std::string& installed_config)
    : app(a), info(std::move(i)), hub(a), messaging(a, endpoints), console(a, *this), http(a, *this) {
    if (info.name.empty()) info.name = "app";
    if (info.config_paths.empty()) {
        info.config_paths.emplace_back(info.name + ".toml");
        if (!installed_config.empty()) info.config_paths.emplace_back(installed_config);
        const auto etc = std::filesystem::path("/etc") / info.name / (info.name + ".toml");
        if (installed_config != etc.string()) info.config_paths.push_back(etc);
    }

    cli.name(info.name);
    cli.description(info.description.empty() ? info.name : info.description);
    cli.set_version_flag("-V,--version", std::format("{} {} (forgecpp {})", info.name, info.version, version_string));
    cli.option_defaults()->group("Framework options");

    cli.add_option("-c,--config", options.config_file, "Configuration file (TOML)");
    cli.add_option("-D,--set", options.overrides, "Override a config value, e.g. -D log.level=debug")
        ->type_name("KEY=VALUE")
        ->allow_extra_args(false);
    cli.add_option("-l,--log-level", options.log_level, "Log level: trace|debug|info|warn|error|critical|off");
    cli.add_flag("-v,--verbose", options.verbosity, "Increase verbosity (-v debug, -vv trace)");
    cli.add_option("--log-file", options.log_file, "Also log to this (rotating) file");
    cli.add_flag("--syslog", options.syslog, "Also log to syslog");
    cli.add_flag("-i,--interactive", options.interactive, "Start the interactive console");
    cli.add_option("--http", options.http_listen, "Enable the HTTP server on [HOST:]PORT")->type_name("[HOST:]PORT");
    cli.add_option("--instance", instance,
                   "Instance name when the application runs several times (default: the instance of its "
                   "systemd template unit <name>@<instance>, or $FORGE_INSTANCE)")
        ->type_name("NAME");
    cli.add_flag("--print-config", options.print_config, "Print the effective configuration as JSON and exit");
    cli.add_flag("--help-config", options.help_config, "Describe every configuration key and exit");
    cli.add_flag("--check-config", options.check_config, "Validate the configuration and exit (status 0 = valid)");

    cli.option_defaults()->group("Options");
}

Application::Impl::~Impl() {
    // Normally torn down by run(); make sure no thread outlives the object.
    console.stop();
    http.stop();
    if (scheduler) scheduler->stop();
    signals.stop();
}

Application::Application(AppInfo info, const std::string& installed_config)
    : impl_(std::make_unique<Impl>(*this, std::move(info), installed_config)) {}

Application::~Application() = default;

// ---------------------------------------------------------------------------
// Lifecycle

int Application::run(int argc, char** argv) { return impl_->run(argc, argv); }

int Application::Impl::run(int argc, char** argv) {
    started_at = std::chrono::steady_clock::now();
    loop.bind_to_this_thread();
    // Before any thread is created: every thread inherits the mask, and only
    // the signal thread takes SIGINT, SIGTERM and SIGHUP.
    signals.block();
    std::signal(SIGPIPE, SIG_IGN);

    if (const auto rc = prepare(argc, argv)) {
        signals.restore();
        return *rc;
    }

    int rc = 0;
    try {
        start();
        loop.run();
        rc = exit_code.load();
    } catch (const std::exception& e) {
        if (logger) {
            logger->critical("fatal: {}", e.what());
        } else {
            std::cerr << info.name << ": fatal: " << e.what() << "\n";
        }
        rc = 1;
    }
    shutdown();
    return rc;
}

std::optional<int> Application::Impl::prepare(int argc, char** argv) {
    if (const int rc = parse_command_line(argc, argv); rc >= 0) return rc;

    // Declared configuration: the framework's keys, the application's, its components'.
    declare_framework_config();
    app.setup_config(schema);
    for (auto& h : hooks.config) h(app, schema);
    for (auto& c : components) ComponentAccess::setup_config(*c, schema);
    if (options.help_config) {
        std::cout << "# " << info.name << " configuration keys (TOML)\n" << schema.help();
        return 0;
    }

    Config cfg;
    try {
        cfg = load_config();
        schema.apply_defaults(cfg);  // the schema is the one place defaults are written
    } catch (const std::exception& e) {
        std::cerr << info.name << ": " << e.what() << "\n";
        return 1;
    }
    {
        std::lock_guard lock(config_mutex);
        config = cfg;
    }
    if (options.print_config) {
        std::cout << cfg.to_json().dump(2) << "\n";
        return 0;
    }
    if (!validate_config(cfg, true)) return 1;
    if (options.check_config) {
        std::cout << info.name << ": configuration is valid\n";
        return 0;
    }

    zmq::set_source(info.name);
    metrics::registry().set_labels({{"app", info.name}});
    try {
        setup_logging(cfg);
    } catch (const std::exception& e) {
        std::cerr << info.name << ": invalid logging configuration: " << e.what() << "\n";
        return 1;
    }
    return std::nullopt;
}

void Application::Impl::setup_logging(const Config& cfg) {
    log::Options o;
    o.format = cfg.value<std::string>("log.format");
    o.history = cfg.value<std::size_t>("log.history");
    o.name = info.name;
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
    logger = log::get();
}

void Application::Impl::start() {
    const Config cfg = snapshot();
    start_core(cfg);
    declare_controls(cfg);

    commands.close();  // the links read the registry from their own threads from here on
    http.create(cfg);
    console.create(cfg);
    messaging.start(cfg);  // events and streams: on_start() may publish

    app.on_start();
    for (auto& c : components) ComponentAccess::on_start(*c);
    for (auto& h : hooks.start) h(app);

    // Requests come in once everything is started: before, one would reach a
    // half-started component (or wait for the main loop in vain).
    messaging.start_control(cfg);
    console.start_telnet(cfg);
    http.start(cfg);
    if (cfg.value<bool>("console.enabled")) console.start_repl();

    start_housekeeping();
    if (hub_registration) hub.start(cfg);
    systemd::notify("READY=1\nSTATUS=running");
    logger->info("{} started", info.name);
}

void Application::Impl::apply_settings(const Config& cfg) {
    loop.configure(cfg.value<int>("loop.request_timeout_ms"), cfg.value<int>("loop.slow_task_ms"),
                   cfg.value<int>("loop.max_pending_tasks"));
    watchdog_grace_s = cfg.value<double>("health.watchdog_grace_s");
}

void Application::Impl::start_core(const Config& cfg) {
    const auto threads = cfg.value<int>("threads.workers");
    pool = std::make_unique<ThreadPool>(threads > 0 ? static_cast<std::size_t>(threads)
                                                    : std::thread::hardware_concurrency());
    // Timer tasks run on the main loop.
    auto to_main_loop = [this](Scheduler::Task t) { loop.post(std::move(t)); };
    scheduler = manual_clock ? std::make_unique<Scheduler>(Scheduler::manual_clock, to_main_loop)
                             : std::make_unique<Scheduler>(to_main_loop);
    loop.track_age(!manual_clock);  // a manual clock only beats when the test advances time
    signals.start([this](int sig) { on_signal(sig); });

    logger->info("{} {} starting (forgecpp {}, pid {}, {} worker threads)", app.id(), info.version, version_string,
                 ::getpid(), pool->get_thread_count());
    if (!is_standard_name(info.name)) {
        logger->warn(
            "'{}' does not follow the daemon naming standard: lowercase words separated by '-' "
            "(e.g. radio-controller); the name is used for the binary, config file, systemd unit "
            "and forge-hub entry",
            info.name);
    }
    if (!cfg.source().empty()) logger->info("configuration: {}", cfg.source().string());
    trace::set_thread_name("main");

    auth.load(cfg);
    apply_settings(cfg);
    start_observability(cfg);
    control = std::make_unique<ControlService>(app, [this](bool persistent) { control_changed(persistent); });
    detail::register_builtin_commands(app, *this);
}

void Application::Impl::declare_controls(const Config& cfg) {
    // The control registry exists with or without HTTP: the console, the links
    // (forgectl) and telnet use it too.
    controls = std::make_unique<Controls>(cfg.value<std::string>("web.title"));
    app.setup_controls(*controls);
    app.setup_web(*controls);  // the old name
    for (auto& c : components) ComponentAccess::setup_controls(*c, *controls);
    for (auto& h : hooks.controls) h(app, *controls);
    controls->bind_to_thread(std::this_thread::get_id());  // its values belong to the main loop
    state.restore(*controls, cfg.value<std::string>("state.file"), logger);
}

void Application::Impl::start_housekeeping() {
    if (auto interval = systemd::watchdog_interval()) {
        app.every(*interval / 2, [this] {
            if (watchdog.tick(health, watchdog_grace_s.load(), logger)) systemd::notify("WATCHDOG=1");
        }, true);
        logger->debug("systemd watchdog enabled ({} ms)",
                      std::chrono::duration_cast<std::chrono::milliseconds>(*interval).count());
    }
    loop.beat();
    app.every(std::chrono::seconds(1), [this] {
        loop.beat();  // /health reports a stalled main loop
        state.save(*controls, logger, false, pool.get());  // the file is written off the loop
        if (state_event_pending.exchange(false)) control_changed(false);
    });
}

void Application::Impl::on_signal(int sig) {
    if (sig == SIGHUP) {
        logger->info("SIGHUP received, reloading configuration");
        app.reload();
        return;
    }
    logger->info("{} received, shutting down", ::strsignal(sig));
    stop_signal = sig;
    if (app.stopping()) {
        // A second signal while shutting down: give up waiting.
        logger->warn("forced exit");
        log::flush();
        std::_Exit(128 + sig);
    }
    app.stop(0);
}

void Application::Impl::reload() {
    Config fresh;
    try {
        fresh = load_config();
        schema.apply_defaults(fresh);
    } catch (const std::exception& e) {
        logger->error("reload failed, keeping the current configuration: {}", e.what());
        return;
    }
    if (!validate_config(fresh, false)) {
        logger->error("reload failed, keeping the current configuration");
        return;
    }
    {
        std::lock_guard lock(config_mutex);
        config = fresh;
    }
    // What is applied again (docs/application.md "Reloading"): the main loop's
    // settings, the watchdog grace time, the log level; the log file is opened
    // again (logrotate: `postrotate kill -HUP`).
    apply_settings(fresh);
    if (auto level = fresh.get<std::string>("log.level")) {
        try {
            log::set_level(*level);
        } catch (const std::exception& e) {
            logger->error("{}", e.what());
        }
    }
    log::reopen();
    try {
        app.on_reload(fresh);
        for (auto& c : components) ComponentAccess::on_reload(*c, fresh);
        for (auto& h : hooks.reload) h(app);
        logger->info("configuration reloaded");
    } catch (const std::exception& e) {
        logger->error("reload handler failed: {}", e.what());
    }
}

void Application::Impl::shutdown() {
    systemd::notify("STOPPING=1");
    if (logger) logger->info("{} stopping", info.name);
    // Tell the hub first, so it does not mistake the end for a crash.
    hub.stop(
        stop_signal.load() != 0 ? forge_hub_unregister_reason_REASON_SIGNAL : forge_hub_unregister_reason_REASON_EXIT,
        exit_code.load());

    console.stop();

    // HTTP and link handlers may be waiting for the main loop (invoke, call):
    // keep running posted tasks while they wind down, so they can finish.
    loop.drain_while([this] { http.stop(); });
    loop.drain_while([this] { messaging.stop_control(); });
    // The application's threads may wait for the main loop too.
    loop.drain_while([this] {
        std::lock_guard lock(workers_mutex);
        workers.clear();
    });

    // Every stop handler runs, also after one of them failed: a faulty
    // component must not leave the others' devices open.
    const auto stop_step = [this](const char* what, const auto& step) {
        try {
            step();
        } catch (const std::exception& e) {
            if (logger) logger->error("{} failed on stop: {}", what, e.what());
        } catch (...) {
            if (logger) logger->error("{} failed on stop: unknown exception", what);
        }
    };
    for (auto it = components.rbegin(); it != components.rend(); ++it) {
        stop_step((*it)->name().c_str(), [&] { ComponentAccess::on_stop(**it); });
    }
    stop_step("on_stop()", [&] { app.on_stop(); });
    for (auto& h : hooks.stop) stop_step("a stop hook", [&] { h(app); });

    if (controls) state.save(*controls, logger, true);
    messaging.stop();
    if (scheduler) scheduler->stop();
    if (pool) loop.drain_while([this] { pool->wait(); });
    signals.stop();

    if (logger) logger->info("{} stopped", info.name);
    log::flush();
    signals.restore();  // the mask as run() found it
    // Tasks that never ran are dropped now, so a thread waiting on a call()
    // future gets std::future_error (a broken promise) instead of waiting for ever.
    loop.discard_pending();

    endpoints.clear();
    hub.reset();
    console.reset();
    http.reset();
    messaging.reset();
    controls.reset();
    scheduler.reset();
    pool.reset();
}

void Application::Impl::control_changed(bool persistent) {
    if (persistent) state.mark_dirty();
    if (!loop.on_loop()) {
        state_event_pending = true;
        loop.post([this] { control_changed(false); });
        return;
    }
    http.push_state();
    // "state" events: the values that changed.
    if (controls && messaging.wants_state()) messaging.publish_state(controls->state());
}

Config Application::Impl::snapshot() const {
    std::lock_guard lock(config_mutex);
    return config;
}

void Application::Impl::setup_console(Console& c) {
    app.setup_console(c);
    for (auto& component : components) ComponentAccess::setup_console(*component, c);
    for (auto& h : hooks.console) h(app, c);
}

void Application::Impl::setup_http(http::Server& server) {
    app.setup_http(server);
    for (auto& component : components) ComponentAccess::setup_http(*component, server);
    for (auto& h : hooks.http) h(app, server);
}

// ---------------------------------------------------------------------------
// The main loop

void Application::stop(int exit_code) {
    // The exit code of the first request counts; it is set before the loop is told.
    if (!impl_->stop_requested.exchange(true)) impl_->exit_code = exit_code;
    impl_->loop.stop();
}

bool Application::stopping() const { return impl_->stop_requested.load(); }

void Application::reload() {
    post([this] { impl_->reload(); });
}

bool Application::post(Task task) { return impl_->loop.post(std::move(task)); }
bool Application::on_main_loop() const { return impl_->loop.on_loop(); }
std::chrono::milliseconds Application::request_timeout() const { return impl_->loop.request_timeout(); }

void Application::invoke_erased(std::optional<std::chrono::steady_clock::time_point> deadline,
                                const std::function<void()>& run) {
    impl_->loop.invoke(deadline, run);
}

void Application::run_in_pool(std::function<void()> fn) { pool().detach_task(std::move(fn)); }

void Application::background_failed(const char* what) { logger()->error("background task failed: {}", what); }

// ---------------------------------------------------------------------------
// Timers and time

Scheduler::Id Application::after(Duration delay, Task task) { return scheduler().after(delay, std::move(task)); }

Scheduler::Id Application::every(Duration period, Task task, bool run_immediately) {
    return scheduler().every(period, std::move(task), run_immediately);
}

bool Application::cancel(Scheduler::Id id) { return impl_->scheduler ? impl_->scheduler->cancel(id) : false; }

Scheduler::Clock::time_point Application::now() const {
    return impl_->scheduler ? impl_->scheduler->now() : Scheduler::Clock::now();
}

void Application::use_manual_clock() { impl_->manual_clock = true; }
bool Application::manual_clock() const { return impl_->manual_clock; }

std::size_t Application::advance_time(Duration d) {
    auto* timers = impl_->scheduler.get();
    if (!timers || !timers->manual()) throw std::logic_error("advance_time() needs use_manual_clock() before run()");
    if (on_main_loop()) throw std::logic_error("advance_time() waits for the main loop: call it from the test");
    const auto target = timers->now() + d;
    std::size_t ran = 0;
    // One due time at a time: what a task schedules (after(), every()) is seen
    // by the next step, and every task reads now() == the time it was due.
    while (auto next = timers->next_due()) {
        if (*next > target) break;
        ran += timers->advance(*next - timers->now());
        invoke([] {});  // the posted tasks have run
    }
    timers->advance(target - timers->now());
    invoke([] {});
    return ran;
}

std::chrono::steady_clock::duration Application::uptime() const {
    return std::chrono::steady_clock::now() - impl_->started_at;
}

// ---------------------------------------------------------------------------
// The application's parts

const AppInfo& Application::info() const { return impl_->info; }
const std::string& Application::instance() const { return impl_->instance; }
std::string Application::id() const {
    return impl_->instance.empty() ? impl_->info.name : impl_->info.name + "@" + impl_->instance;
}

Config Application::config() const { return impl_->snapshot(); }
Logger Application::logger() const { return impl_->logger ? impl_->logger : log::get(); }
CLI::App& Application::cli() { return impl_->cli; }

ThreadPool& Application::pool() {
    if (!impl_->pool) throw std::logic_error("Application::pool() used before run()");
    return *impl_->pool;
}

Scheduler& Application::scheduler() {
    if (!impl_->scheduler) throw std::logic_error("Application::scheduler() used before run()");
    return *impl_->scheduler;
}

ControlService& Application::control() {
    if (!impl_->control) throw std::logic_error("control() is available from setup_controls() on");
    return *impl_->control;
}

EventBus& Application::events() { return impl_->events; }
Alarms& Application::alarms() { return impl_->alarms; }
Audit& Application::audit() { return impl_->audit; }
Health& Application::health() { return impl_->health; }
const Auth& Application::auth() const { return impl_->auth; }
tmtc::Registry& Application::commands() { return impl_->commands; }
web::Dashboard* Application::dashboard() { return impl_->controls.get(); }
http::Server* Application::http() { return impl_->http.server(); }
Console* Application::console() { return impl_->console.console(); }

void Application::disable_hub_registration() { impl_->hub_registration = false; }

void Application::publish_web(const std::string& event, const json& data) { impl_->http.publish(event, data); }

void Application::notify_web(const std::string& message, const std::string& level) {
    publish_web("notify", json{{"message", message}, {"level", level}});
}

zmq::Stream& Application::stream(const std::string& name, const pb::Type& type) {
    return impl_->messaging.stream(name, type, zmq::StreamOptions{});
}

zmq::Stream& Application::stream(const std::string& name, const pb::Type& type, const zmq::StreamOptions& o) {
    return impl_->messaging.stream(name, type, o);
}

Status Application::run_command(const std::string& name, Role role, const std::function<json()>& request,
                                const std::function<Status()>& run) {
    return control().run_command(name, role, request, run, current_caller());
}

std::map<std::string, std::string> Application::endpoints() const { return impl_->endpoints.all(); }

void Application::attach(std::unique_ptr<Component> component) {
    ComponentAccess::attach(*component, *this);
    impl_->components.push_back(std::move(component));
}

Worker& Application::worker(const std::string& name, Worker::Body body) {
    // A worker is a component of health: while it runs, ok; when its body
    // throws, failed and critical, so that the watchdog lets systemd restart a
    // program whose thread is gone (and /health, the hub and the panel say so).
    const auto component = "worker." + name;
    health().set(component, HealthState::ok, "running", true);
    auto w = std::make_unique<Worker>(name, [this, name, body = std::move(body)](std::stop_token stop) {
        configure_thread(name);
        body(std::move(stop));
    }, [this, name, component](const std::string& why) {
        alarms().raise(component, Severity::error, "thread " + name + " ended: " + why);
        health().set(component, HealthState::failed, "ended: " + why, true);
    });
    std::lock_guard lock(impl_->workers_mutex);
    impl_->workers.push_back(std::move(w));
    return *impl_->workers.back();
}

std::string Application::configure_thread(const std::string& name) {
    trace::set_thread_name(name);
    crash::use_alternate_stack();
    const auto settings = rt::thread_settings(config(), name);
    if (!settings) return {};
    // Everything that allocates happens before the priority goes up: the log
    // call (the first one on a thread creates its queue) and the queue's
    // preallocation. After rt::apply() the thread only logs.
    logger()->info("thread {}: applying [threads.{}]", name, name);
    log::preallocate();
    const auto err = rt::apply(*settings);
    if (err.empty()) {
        logger()->info("thread {}: {}", name, rt::describe_current_thread());
    } else {
        logger()->warn("thread {}: {}", name, err);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Hooks: the lambda forms, and the virtuals' defaults

Application& Application::hook_start(Hook hook) {
    impl_->hooks.start.push_back(std::move(hook));
    return *this;
}
Application& Application::hook_stop(Hook hook) {
    impl_->hooks.stop.push_back(std::move(hook));
    return *this;
}
Application& Application::hook_reload(Hook hook) {
    impl_->hooks.reload.push_back(std::move(hook));
    return *this;
}
Application& Application::hook_config(std::function<void(Application&, config::Schema&)> hook) {
    impl_->hooks.config.push_back(std::move(hook));
    return *this;
}
Application& Application::hook_cli(std::function<void(Application&, CLI::App&)> hook) {
    impl_->hooks.cli.push_back(std::move(hook));
    return *this;
}
Application& Application::hook_http(std::function<void(Application&, http::Server&)> hook) {
    impl_->hooks.http.push_back(std::move(hook));
    return *this;
}
Application& Application::hook_console(std::function<void(Application&, Console&)> hook) {
    impl_->hooks.console.push_back(std::move(hook));
    return *this;
}
Application& Application::hook_controls(std::function<void(Application&, Controls&)> hook) {
    impl_->hooks.controls.push_back(std::move(hook));
    return *this;
}
Application& Application::hook_web(std::function<void(Application&, web::Dashboard&)> hook) {
    return hook_controls(std::move(hook));
}

void Application::setup_cli(CLI::App&) {}
void Application::setup_config(config::Schema&) {}
void Application::setup_http(http::Server&) {}
void Application::setup_console(Console&) {}
void Application::setup_web(web::Dashboard&) {}
void Application::setup_controls(Controls&) {}
void Application::on_start() {}
void Application::on_stop() {}
void Application::on_reload(const Config&) {}

bool is_standard_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || !std::islower(static_cast<unsigned char>(name.front())) ||
        name.back() == '-') {
        return false;
    }
    char prev = 0;
    for (const char c : name) {
        const bool ok =
            std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '-';
        if (!ok || (c == '-' && prev == '-')) return false;
        prev = c;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Component

Component::Component(std::string name, bool critical) : name_(std::move(name)), critical_(critical) {}
Component::~Component() = default;
void Component::setup_config(config::Schema&) {}
void Component::setup_web(web::Dashboard&) {}
void Component::setup_controls(Controls&) {}
void Component::setup_console(Console&) {}
void Component::setup_http(http::Server&) {}
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
