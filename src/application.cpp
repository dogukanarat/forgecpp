#include "forge/application.hpp"

#include "app/services.hpp"

#include "forge/channel.hpp"
#include "forge/console.hpp"
#include "forge/control.hpp"
#include "forge/crash.hpp"
#include "forge/http.hpp"
#include "forge/hub.hpp"
#include "forge/rt.hpp"
#include "forge/systemd.hpp"
#include "forge/thread_pool.hpp"
#include "forge/trace.hpp"
#include "forge/version.hpp"
#include "forge/web.hpp"
#include "forge/zmq.hpp"

#include <CLI/CLI.hpp>

#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace forge {

namespace detail {

/// An exception on its way from the main loop to a thread waiting in
/// Application::invoke(): described here, thrown anew on the waiting thread.
struct LoopError {
    int kind = 0;  ///< 0 = no error
    int status = 0;
    std::string message;
    json body;
    explicit operator bool() const { return kind != 0; }
    /// Describe the exception being handled (call inside a catch block).
    void capture() noexcept;
    void set_stopped();
    [[noreturn]] void rethrow() const;
};

namespace {
enum LoopErrorKind { kNone, kHttp, kConfig, kInvalidArgument, kOutOfRange, kRuntime, kStopped };
}  // namespace

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
    : info_(std::move(info)), cli_(std::make_unique<CLI::App>()), tasks_(std::make_unique<Channel<Task>>(256)) {
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
    cli.set_version_flag("-V,--version", std::format("{} {} (forgecpp {})", info_.name, info_.version, version_string));
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
    tasks_->push([] {});  // wake the main loop
}

void Application::background_failed(const char* what) {
    if (logger_) logger_->error("background task failed: {}", what);
}

void Application::run_in_pool(std::function<void()> fn) { pool().detach_task(std::move(fn)); }

void Application::invoke_erased(std::optional<std::chrono::steady_clock::time_point> deadline,
                                const std::function<void()>& run) {
    enum class Phase { pending, running, done, abandoned };
    struct Waiter {
        std::mutex mutex;
        std::condition_variable cv;
        Phase phase = Phase::pending;
        detail::LoopError error;
    };
    // Finishes the wait even when the task is discarded (application stopping).
    struct Guard {
        explicit Guard(std::shared_ptr<Waiter> waiter) : w(std::move(waiter)) {}
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        std::shared_ptr<Waiter> w;
        ~Guard() {
            bool waiting;
            {
                std::lock_guard lock(w->mutex);
                waiting = w->phase == Phase::pending || w->phase == Phase::running;
                if (waiting) {
                    w->error.set_stopped();
                    w->phase = Phase::done;
                }
            }
            if (waiting) w->cv.notify_all();
        }
    };
    auto w = std::make_shared<Waiter>();
    auto guard = std::make_shared<Guard>(w);
    // `run` refers to the caller's frame: it is only called while the caller
    // waits (phase running), never after the caller gave up (abandoned).
    const bool posted = post([w, &run, guard] {
        {
            std::lock_guard lock(w->mutex);
            if (w->phase == Phase::abandoned) return;  // the caller gave up: do not run
            w->phase = Phase::running;
        }
        detail::LoopError error;
        try {
            run();
        } catch (...) {
            error.capture();
        }
        {
            std::lock_guard lock(w->mutex);
            w->error = std::move(error);
            w->phase = Phase::done;
        }
        w->cv.notify_all();
    });
    guard.reset();
    if (!posted) loop_busy();  // the queue is full: nothing was queued, nothing to wait for
    std::unique_lock lock(w->mutex);
    const auto finished = [&] { return w->phase == Phase::done; };
    if (deadline && !w->cv.wait_until(lock, *deadline, finished) && w->phase == Phase::pending) {
        w->phase = Phase::abandoned;
        lock.unlock();
        loop_busy();
    }
    w->cv.wait(lock, finished);
    if (w->error) w->error.rethrow();
}

void Application::loop_busy() const {
    static auto& timeouts =
        metrics::registry().counter("forge.control.timeouts", "Requests that gave up waiting for a busy main loop");
    timeouts.inc();
    const auto lag = loop_age_ms();
    throw Error(503, std::format("main loop busy (last heartbeat {} ms ago)", lag),
                json{{"error", "main loop busy"}, {"loop_age_ms", lag}, {"timeout_ms", request_timeout_ms_.load()}})
        .module(module_id::tmtc);  // forgecpp refuses, not the command
}

void Application::reload() {
    post([this] { do_reload(); });
}

bool Application::post(Task task) {
    if (!task) return false;
    // Main loop lag: time from post() to execution (a busy loop shows up here first).
    static auto& lag = metrics::registry().histogram("forge.loop.lag_seconds", metrics::latency_buckets(),
                                                     "Delay between post() and execution on the main loop", "s");
    static auto& busy = metrics::registry().counter("forge.loop.busy_us", "Time spent running main loop tasks", "us");
    static auto& slow = metrics::registry().counter("forge.loop.slow_tasks",
                                                    "Main loop tasks that took longer than [loop].slow_task_ms");
    static auto& dropped = metrics::registry().counter(
        "forge.loop.dropped", "Tasks refused because the main loop's queue was full ([loop].max_pending_tasks)");
    // Bounded: a loop that is stuck must not let the producers fill the memory.
    const auto limit = static_cast<std::size_t>(std::max(max_pending_tasks_.load(std::memory_order_relaxed), 1));
    if (tasks_->size_approx() >= limit) {
        dropped.inc();
        static std::atomic<std::int64_t> last_warning{0};
        const auto now_s =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        if (now_s - last_warning.exchange(now_s) >= 10 && logger_) {
            logger_->error(
                "main loop queue full ({} tasks waiting): dropping tasks (forge.loop.dropped); the loop "
                "is stuck or far behind",
                limit);
        }
        return false;
    }
    return tasks_->push([this, t = std::move(task), queued = std::chrono::steady_clock::now()] {
        const auto start = std::chrono::steady_clock::now();
        lag.observe(std::chrono::duration<double>(start - queued).count());
        FORGE_TRACE_SCOPE("loop.task", "forge");
        t();
        const auto took =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
        busy.inc(static_cast<std::uint64_t>(took.count()));
        // Everything else waits while a task runs: point at it (at most every 10 s).
        const int limit_ms = slow_task_ms_.load(std::memory_order_relaxed);
        if (limit_ms > 0 && took > std::chrono::milliseconds(limit_ms)) {
            slow.inc();
            static std::atomic<std::int64_t> last_warning{0};
            const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(start.time_since_epoch()).count();
            if (now_s - last_warning.exchange(now_s) >= 10 && logger_) {
                logger_->warn(
                    "a main loop task took {} ms (> [loop].slow_task_ms = {}): everything else waited; "
                    "move heavy work off the loop with background() (forge.loop.slow_tasks, trace \"loop.task\")",
                    took.count() / 1000, limit_ms);
            }
        }
    });
}

Scheduler::Id Application::after(Duration delay, Task task) { return scheduler().after(delay, std::move(task)); }

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
Application& Application::hook_controls(std::function<void(Application&, Controls&)> hook) {
    hooks_.web.push_back(std::move(hook));
    return *this;
}
Application& Application::hook_web(std::function<void(Application&, web::Dashboard&)> hook) {
    return hook_controls(std::move(hook));
}
Application& Application::hook_config(std::function<void(Application&, config::Schema&)> hook) {
    hooks_.config.push_back(std::move(hook));
    return *this;
}
Application& Application::hook_cli(std::function<void(Application&, CLI::App&)> hook) {
    hooks_.cli.push_back(std::move(hook));
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
void Application::setup_controls(Controls&) {}

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
    const auto now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
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
    pthread_sigmask(SIG_BLOCK, &set, &saved_signal_mask_);  // shutdown() puts it back
    std::signal(SIGPIPE, SIG_IGN);

    if (int rc = parse_command_line(argc, argv); rc >= 0) return rc;

    // Declared configuration: framework keys, the application's, its components'.
    declare_framework_config(schema_);
    setup_config(schema_);
    for (auto& h : hooks_.config) h(*this, schema_);
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

        logger_->info("{} {} starting (forgecpp {}, pid {}, {} worker threads)", id(), info_.version, version_string,
                      ::getpid(), pool_->get_thread_count());
        if (!is_standard_name(info_.name)) {
            logger_->warn(
                "'{}' does not follow the daemon naming standard: lowercase words separated by '-' "
                "(e.g. radio-controller); the name is used for the binary, config file, systemd unit "
                "and forge-hub entry",
                info_.name);
        }
        if (!cfg.source().empty()) logger_->info("configuration: {}", cfg.source().string());
        trace::set_thread_name("main");

        auth_.load(cfg);
        request_timeout_ms_ = cfg.value<int>("loop.request_timeout_ms");
        slow_task_ms_ = cfg.value<int>("loop.slow_task_ms");
        max_pending_tasks_ = cfg.value<int>("loop.max_pending_tasks");
        start_observability(cfg);
        control_ = std::make_unique<ControlService>(*this);
        detail::register_builtin_commands(*this);

        // The control registry exists even without HTTP: the console and
        // other channels (ZeroMQ, forgectl, telnet) use it too.
        dashboard_ = std::make_unique<web::Dashboard>(cfg.value<std::string>("web.title"));
        setup_controls(*dashboard_);
        setup_web(*dashboard_);  // the old name
        for (auto& c : components_) {
            c->setup_controls(*dashboard_);
            c->setup_web(*dashboard_);
        }
        for (auto& h : hooks_.web) h(*this, *dashboard_);
        dashboard_->bind_to_thread(main_thread_.load());  // its values belong to the main loop
        state_->restore(*dashboard_, cfg.value<std::string>("state.file"), logger_);

        commands_closed_ = true;
        commands_.close();  // the links read it from their threads from here on
        http_->create(cfg);
        console_->create(cfg);
        messaging_->start(cfg);  // events and streams: on_start() may publish

        on_start();
        for (auto& c : components_) c->on_start();
        for (auto& h : hooks_.start) h(*this);

        // Commands come in once everything is started: before, a request would
        // reach a half-started component (or wait for the main loop in vain).
        messaging_->start_control(cfg);
        console_->start_telnet(cfg);
        http_->start(cfg);
        if (cfg.value<bool>("console.enabled")) console_->start_repl();

        watchdog_grace_s_ = cfg.value<double>("health.watchdog_grace_s");
        if (auto wd = systemd::watchdog_interval()) {
            every(*wd / 2, [this] {
                if (watchdog_->tick(health_, watchdog_grace_s_.load(), logger_)) systemd::notify("WATCHDOG=1");
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
            state_->save(*dashboard_, logger_, false, &pool());  // the file is written off the loop
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
    signal_wake_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    signal_thread_ = std::thread([this] {
        // Asleep until a signal (signalfd on the blocked set) or stop_signal_thread()
        // (the eventfd): no periodic wake-ups.
        const sigset_t set = handled_signal_set();
        const int sfd = ::signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);
        pollfd fds[] = {{sfd, POLLIN, 0}, {signal_wake_fd_, POLLIN, 0}};
        while (signal_thread_running_.load()) {
            if (::poll(fds, 2, -1) <= 0) continue;  // EINTR
            if (!(fds[0].revents & POLLIN)) continue;
            signalfd_siginfo info{};
            if (::read(sfd, &info, sizeof(info)) != static_cast<ssize_t>(sizeof(info))) continue;
            const int sig = static_cast<int>(info.ssi_signo);
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
        ::close(sfd);
    });
}

void Application::stop_signal_thread() {
    signal_thread_running_ = false;
    if (signal_wake_fd_ >= 0) {
        const std::uint64_t one = 1;
        (void)!::write(signal_wake_fd_, &one, sizeof(one));
    }
    if (signal_thread_.joinable()) signal_thread_.join();
    if (signal_wake_fd_ >= 0) {
        ::close(signal_wake_fd_);
        signal_wake_fd_ = -1;
    }
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
        tasks_->push([] {});
    });
    while (!done.load()) {
        auto task = tasks_->pop_for(std::chrono::milliseconds(50));
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
        // stop() pushes a task, so the wait only ends when there is something to do.
        auto task = tasks_->pop_for(std::chrono::seconds(5));
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
    // What is applied again (docs/application.md "Reloading"): the main loop
    // settings, the log level, the watchdog grace time, and the log file is
    // opened again (logrotate: `postrotate kill -HUP`).
    request_timeout_ms_ = fresh.value<int>("loop.request_timeout_ms");
    slow_task_ms_ = fresh.value<int>("loop.slow_task_ms");
    max_pending_tasks_ = fresh.value<int>("loop.max_pending_tasks");
    watchdog_grace_s_ = fresh.value<double>("health.watchdog_grace_s");
    if (auto level = fresh.get<std::string>("log.level")) {
        try {
            log::set_level(*level);
        } catch (const std::exception& e) {
            logger_->error("{}", e.what());
        }
    }
    log::reopen();
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

    // The application's threads may wait on the main loop too.
    drain_while([this] {
        std::lock_guard lock(workers_mutex_);
        workers_.clear();
    });

    // Every stop handler runs, also after one of them failed: a faulty
    // component must not leave the others' devices open.
    const auto stop_step = [this](const char* what, const auto& run) {
        try {
            run();
        } catch (const std::exception& e) {
            if (logger_) logger_->error("{} failed on stop: {}", what, e.what());
        } catch (...) {
            if (logger_) logger_->error("{} failed on stop: unknown exception", what);
        }
    };
    for (auto it = components_.rbegin(); it != components_.rend(); ++it) {
        stop_step((*it)->name().c_str(), [&] { (*it)->on_stop(); });
    }
    stop_step("on_stop()", [&] { on_stop(); });
    for (auto& h : hooks_.stop) stop_step("a stop hook", [&] { h(*this); });
    if (dashboard_) state_->save(*dashboard_, logger_, true);
    messaging_->stop();

    if (scheduler_) scheduler_->stop();
    if (pool_) drain_while([this] { pool_->wait(); });
    stop_signal_thread();

    if (logger_) logger_->info("{} stopped", info_.name);
    log::flush();
    pthread_sigmask(SIG_SETMASK, &saved_signal_mask_, nullptr);  // as run() found it
    // Tasks that never ran: dropped now, so a thread waiting on a call() future
    // gets std::future_error (broken promise) instead of waiting for ever.
    while (tasks_->try_pop()) {
    }

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

Status Application::run_command(const std::string& name, Role role, const std::function<json()>& request,
                                const std::function<Status()>& run) {
    return control().run_command(name, role, request, run, current_caller());
}

std::vector<zmq::StreamInfo> Application::stream_list() const { return messaging_->stream_list(); }

std::map<std::string, std::string> Application::endpoints() const {
    std::map<std::string, std::string> e;
    messaging_->add_endpoints(e);
    http_->add_endpoints(e);
    console_->add_endpoints(e);
    return e;
}

Worker& Application::worker(const std::string& name, Worker::Body body) {
    // A worker is a component of health: while it runs, ok; when its body
    // throws, failed and critical, so that the watchdog lets systemd restart a
    // program whose thread is gone (and /health, the hub and the panel say so).
    const auto component = "worker." + name;
    health_.set(component, HealthState::ok, "running", true);
    auto w = std::make_unique<Worker>(name, [this, name, body = std::move(body)](std::stop_token stop) {
        configure_thread(name);
        body(std::move(stop));
    }, [this, name, component](const std::string& why) {
        alarms().raise(component, Severity::error, "thread " + name + " ended: " + why);
        health_.set(component, HealthState::failed, "ended: " + why, true);
    });
    std::lock_guard lock(workers_mutex_);
    workers_.push_back(std::move(w));
    return *workers_.back();
}

std::string Application::configure_thread(const std::string& name) {
    trace::set_thread_name(name);
    crash::use_alternate_stack();
    const auto settings = rt::thread_settings(config(), name);
    if (!settings) return {};
    // Everything that allocates happens before the priority goes up: the
    // log call (the first one on a thread creates its queue) and the queue's
    // preallocation. After rt::apply() the thread only logs.
    logger_->info("thread {}: applying [threads.{}]", name, name);
    log::preallocate();
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

// ---------------------------------------------------------------------------
// Component

Component::Component(std::string name, bool critical) : name_(std::move(name)), critical_(critical) {}
Component::~Component() = default;
void Component::setup_config(config::Schema&) {}
void Component::setup_web(web::Dashboard&) {}
void Component::setup_controls(Controls&) {}
void Component::setup_console(Console&) {}
void Component::setup_http(http::Server&) {}
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
