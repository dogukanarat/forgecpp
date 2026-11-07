#pragma once
// forge::Application - the skeleton every daemon shares.
//
// run() gives you, in this order:
//   1. command line parsing (CLI11) with the standard daemon flags
//   2. TOML configuration (file + `-D key=value` overrides + flags)
//   3. logging (console / rotating file / syslog) configured from [log]
//   4. a thread pool sized from [threads].workers
//   5. a main loop (post / call / timers / events all run on it)
//   6. optional HTTP server ([http] or --http) with GET /health, your own
//      routes, your embedded web pages and the built-in control dashboard
//      (/forge/) with live updates
//   7. console commands on the local terminal (-i) and over telnet
//      ([console].telnet), as a backup to the web UI (same controls)
//   8. signal handling: SIGINT/SIGTERM stop, SIGHUP reloads the config
//   9. systemd notify + watchdog support (Type=notify units), fed by health
//  10. the forge.v1 standard on ZeroMQ: control ([forge].control, forgectl and
//      remote panels), events ([forge].events: state, alarms, logs, audit,
//      health, metrics) and the application's streams ([forge].data)
//  11. observability: metrics (/metrics), flight recorder (trace), audit
//      trail, crash reports, JSON logs; declared configuration (schema)
//
// Subclass style:
//
//   class MyDaemon : public forge::Application {
//   public:
//       MyDaemon() : Application({"mydaemon", "1.0.0", "Does useful things"}) {}
//   protected:
//       void on_start() override { every(1s, [this] { tick(); }); }
//   };
//   FORGE_MAIN(MyDaemon)
//
// Lambda style:
//
//   int main(int argc, char** argv) {
//       forge::Application app({"hello", "1.0.0"});
//       app.hook_start([](forge::Application& a) { a.logger()->info("started"); });
//       return app.run(argc, argv);
//   }

#include "forge/alarms.hpp"
#include "forge/audit.hpp"
#include "forge/auth.hpp"
#include "forge/channel.hpp"
#include "forge/component.hpp"
#include "forge/config.hpp"
#include "forge/config_schema.hpp"
#include "forge/events.hpp"
#include "forge/health.hpp"
#include "forge/json.hpp"
#include "forge/log.hpp"
#include "forge/metrics.hpp"
#include "forge/scheduler.hpp"
#include "forge/thread_pool.hpp"
#include "forge/v1/control.pb.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#ifndef FORGE_APP_INSTALLED_CONFIG
#define FORGE_APP_INSTALLED_CONFIG ""
#endif

namespace CLI {
class App;
}

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
}  // namespace detail

namespace http {
class Server;
class EventStream;
}  // namespace http
namespace detail {
class StateStore;
class Watchdog;
class HubRegistration;
class Messaging;
class RemoteConsole;
class HttpFrontend;
}  // namespace detail
namespace web {
class Dashboard;
}
namespace zmq {
class Publisher;
class Server;
class Stream;
struct StreamOptions;
}  // namespace zmq
class Console;
class ControlService;
class Telnet;
namespace hub {
class Client;
}

/// Daemon naming standard: lowercase letters and digits, words separated by '-'
/// ("radio-controller", "forge-hub"). Other names work, with a warning at start-up.
bool is_standard_name(std::string_view name);

struct AppInfo {
    AppInfo(std::string name_ = "app", std::string version_ = "0.1.0", std::string description_ = {},
            std::vector<std::filesystem::path> config_paths_ = {})
        : name(std::move(name_)),
          version(std::move(version_)),
          description(std::move(description_)),
          config_paths(std::move(config_paths_)) {}

    /// Daemon name: lowercase words separated by '-' (radio-controller). It names
    /// the binary, the logger, <name>.toml, <name>.service and the forge-hub entry.
    std::string name;
    std::string version;
    std::string description;
    /// Searched in order when --config is not given. Empty = defaults:
    /// ./<name>.toml, <installed config>, /etc/<name>/<name>.toml
    std::vector<std::filesystem::path> config_paths;
};

class Application {
public:
    using Task = std::function<void()>;
    using Hook = std::function<void(Application&)>;
    using Duration = Scheduler::Clock::duration;

    /// `installed_config` is where the build system installs this app's config
    /// file; forge_add_app() defines FORGE_APP_INSTALLED_CONFIG accordingly.
    explicit Application(AppInfo info, std::string installed_config = FORGE_APP_INSTALLED_CONFIG);
    virtual ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /// Run the whole lifecycle and return the process exit code.
    int run(int argc, char** argv);

    /// Request shutdown (thread-safe, also from signal context via the signal thread).
    void stop(int exit_code = 0);
    bool stopping() const { return stop_requested_.load(); }

    /// Re-read the configuration file and call on_reload() (thread-safe).
    void reload();

    // --- main loop --------------------------------------------------------
    /// Run `task` on the main loop thread (thread-safe).
    void post(Task task);

    /// Run `fn` on the main loop and wait for its result. Runs inline when
    /// already on the main loop, so it is safe everywhere (HTTP handlers,
    /// worker threads, console commands).
    /// Exceptions thrown by `fn` are thrown again on the calling thread
    /// (forge::Error with its status and details, thrown as http::HttpError,
    /// which is a forge::Error; ConfigError, std::invalid_argument,
    /// std::out_of_range, otherwise std::runtime_error with the message): a new
    /// object is thrown there, none is shared between threads.
    template <class F>
    auto invoke(F&& fn) -> std::invoke_result_t<std::decay_t<F>> {
        return invoke_until(std::nullopt, std::forward<F>(fn));
    }

    /// invoke() that gives up when the main loop has not started `fn` within
    /// `timeout`: throws forge::Error 503 "main loop busy" (with the loop's
    /// lag) and `fn` never runs. Once `fn` has started it is waited for, so it
    /// may refer to the caller's locals. The framework's request handlers
    /// (forge.v1, HTTP) use it with request_timeout().
    template <class F>
    auto invoke_for(std::chrono::milliseconds timeout, F&& fn) -> std::invoke_result_t<std::decay_t<F>> {
        return invoke_until(std::chrono::steady_clock::now() + timeout, std::forward<F>(fn));
    }

    /// [forge].request_timeout_ms: how long a request waits for the main loop.
    std::chrono::milliseconds request_timeout() const {
        return std::chrono::milliseconds(request_timeout_ms_.load());
    }

    /// True on the thread running the main loop.
    bool on_main_loop() const { return std::this_thread::get_id() == main_thread_.load(); }

    /// Run `fn` on the main loop and get its result as a future. On the main
    /// loop itself it runs at once (a ready future), so waiting on it never
    /// deadlocks.
    template <class F>
    auto call(F&& fn) -> std::future<std::invoke_result_t<std::decay_t<F>>> {
        using R = std::invoke_result_t<std::decay_t<F>>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        auto future = task->get_future();
        if (on_main_loop()) {
            (*task)();
        } else {
            post([task] { (*task)(); });
        }
        return future;
    }

    /// Heavy work off the main loop: `work` runs on the worker pool, then
    /// `then(result)` on the main loop (where the application's state may be
    /// touched). An exception from `work` is logged, `then` is not called.
    ///
    ///   background([path] { return load_big_file(path); },
    ///              [this](Data d) { data_ = std::move(d); });
    template <class Work, class Then>
    void background(Work work, Then then) {
        using R = std::invoke_result_t<Work>;
        pool().detach_task([this, work = std::move(work), then = std::move(then)]() mutable {
            try {
                if constexpr (std::is_void_v<R>) {
                    work();
                    post([then = std::move(then)]() mutable { then(); });
                } else {
                    auto result = std::make_shared<R>(work());
                    post([then = std::move(then), result]() mutable { then(std::move(*result)); });
                }
            } catch (const std::exception& e) {
                background_failed(e.what());
            } catch (...) {
                background_failed("unknown exception");
            }
        });
    }

    /// Timers; callbacks run on the main loop.
    /// Time-based logic should read now() (not steady_clock) so tests can drive it.
    Scheduler::Id after(Duration delay, Task task);
    Scheduler::Id every(Duration period, Task task, bool run_immediately = false);
    bool cancel(Scheduler::Id id);
    /// The application's time: steady_clock, or the manual clock of a test.
    Scheduler::Clock::time_point now() const;

    // --- deterministic time for tests ----------------------------------------
    /// Before run(): timers only fire when the test calls advance_time()
    /// (forge::testing::AppRunner: Options{.manual_time = true}, r.advance(10s)).
    void use_manual_clock() { manual_clock_ = true; }
    bool manual_clock() const { return manual_clock_; }
    /// Manual clock only, not from the main loop: move time forward by `d`.
    /// Every timer that falls due on the way runs on the main loop, in order,
    /// seeing now() == its due time, before the next one; returns how many ran.
    std::size_t advance_time(Duration d);

    /// Publish an event asynchronously on the main loop.
    template <class Event>
    void emit(Event event) {
        post([this, e = std::move(event)] { events_.publish(e); });
    }

    // --- services ---------------------------------------------------------
    const AppInfo& info() const { return info_; }
    /// Instance name when the application runs several times on a device
    /// (--instance, or the instance of its systemd template unit <name>@<instance>);
    /// "" otherwise. Valid from run() on.
    const std::string& instance() const { return instance_; }
    /// "<name>" or "<name>@<instance>": how forge-hub, the logs and /forge/api/info name it.
    std::string id() const { return instance_.empty() ? info_.name : info_.name + "@" + instance_; }
    /// Snapshot of the effective configuration (file + overrides + flags).
    Config config() const;
    Logger logger() const;
    /// Valid from on_start() until run() returns.
    ThreadPool& pool();
    EventBus& events() { return events_; }
    /// Valid from on_start() until run() returns.
    Scheduler& scheduler();
    /// nullptr unless the HTTP server is enabled.
    http::Server* http();
    /// Control registry declared in setup_web(); valid from on_start() until run()
    /// returns, with or without HTTP. Served at /forge/ when the web UI is enabled,
    /// and by the console and forge::zmq::serve_application().
    web::Dashboard* dashboard() { return dashboard_.get(); }
    /// Alarm list shown in the web UI (bell, banner, panel) and the console
    /// (`alarms`, `ack`). Thread-safe.
    Alarms& alarms() { return alarms_; }
    /// Push an event to every open web page: forge.on(event, data => ...) in the browser.
    void publish_web(const std::string& event, const json& data);
    /// Short-lived message on every open page (level: info|success|warning|error).
    /// For conditions that need operator attention use alarms() instead.
    void notify_web(const std::string& message, const std::string& level = "info");
    /// Console command registry (used by -i and telnet); valid from start-up.
    Console* console();
    /// The single path for control changes (roles, leases, audit); see control.hpp.
    ControlService& control();
    /// Who changed what (control changes, presses, commands, acks, leases).
    Audit& audit() { return audit_; }
    /// Component health; drives /health, the watchdog and the "health" events.
    Health& health() { return health_; }
    const Auth& auth() const { return auth_; }
    /// Process-wide metrics (forge::metrics::registry()).
    metrics::Registry& metrics() { return metrics::registry(); }

    /// A stream of `type` messages on [forge].data (created on first use;
    /// thread-safe publish). See forge/zmq.hpp.
    zmq::Stream& stream(const std::string& name, const std::string& type);
    zmq::Stream& stream(const std::string& name, const std::string& type, const zmq::StreamOptions& options);
    v1::StreamList stream_list() const;
    /// Endpoints this application serves (control, events, data, http, telnet).
    std::map<std::string, std::string> endpoints() const;

    /// Apply [threads.<name>] (policy, priority, cpus) to the calling thread and
    /// name it for the trace view. Returns "" or what could not be applied.
    std::string configure_thread(const std::string& name);

    /// Add a component (before run()); see component.hpp.
    template <class T, class... A>
    T& add_component(A&&... args) {
        auto c = std::make_unique<T>(std::forward<A>(args)...);
        T& ref = *c;
        attach(std::move(c));
        return ref;
    }

    /// Called by ControlService after a control changed (live updates,
    /// persistence, "state" event). Main loop or any thread.
    void control_changed(const std::string& id, bool persistent);
    /// Command line parser; add your own options in the constructor or setup_cli().
    CLI::App& cli() { return *cli_; }
    std::chrono::steady_clock::duration uptime() const;

    // --- lambda hooks (alternative to overriding the virtuals) ------------
    Application& hook_start(Hook hook);
    Application& hook_stop(Hook hook);
    Application& hook_reload(Hook hook);
    Application& hook_http(std::function<void(Application&, http::Server&)> hook);
    Application& hook_console(std::function<void(Application&, Console&)> hook);
    Application& hook_web(std::function<void(Application&, web::Dashboard&)> hook);

protected:
    /// Add command line options (called before parsing).
    virtual void setup_cli(CLI::App& cli);
    /// Declare configuration keys (types, ranges, docs); see config_schema.hpp.
    virtual void setup_config(config::Schema& schema);
    /// Applications that must not register with forge-hub (the hub itself).
    void disable_hub_registration() { hub_registration_ = false; }
    /// Register HTTP routes (called before the server starts listening).
    virtual void setup_http(http::Server& server);
    /// Register console commands.
    virtual void setup_console(Console& console);
    /// Declare web dashboard controls (groups, buttons, switches, sliders, values, links).
    virtual void setup_web(web::Dashboard& dashboard);
    /// Called on the main loop once everything is set up. Throw to abort startup.
    virtual void on_start();
    /// Called on the main loop during shutdown.
    virtual void on_stop();
    /// Called on the main loop after the configuration was reloaded.
    virtual void on_reload(const Config& config);

private:
    template <class F>
    auto invoke_until(std::optional<std::chrono::steady_clock::time_point> deadline, F&& fn)
        -> std::invoke_result_t<std::decay_t<F>> {
        using R = std::invoke_result_t<std::decay_t<F>>;
        if (on_main_loop()) return fn();
        enum class Phase { pending, running, done, abandoned };
        struct Waiter {
            std::mutex mutex;
            std::condition_variable cv;
            Phase phase = Phase::pending;
            // references are kept as pointers
            using Stored = std::conditional_t<std::is_reference_v<R>, std::remove_reference_t<R>*, R>;
            std::conditional_t<std::is_void_v<R>, bool, std::optional<Stored>> value{};
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
        auto f = std::make_shared<std::decay_t<F>>(std::forward<F>(fn));
        auto guard = std::make_shared<Guard>(w);
        post([w, f, guard] {
            {
                std::lock_guard lock(w->mutex);
                if (w->phase == Phase::abandoned) return;  // the caller gave up: do not run
                w->phase = Phase::running;
            }
            detail::LoopError error;
            try {
                if constexpr (std::is_void_v<R>) {
                    (*f)();
                } else if constexpr (std::is_reference_v<R>) {
                    auto* r = &(*f)();
                    std::lock_guard lock(w->mutex);
                    w->value.emplace(r);
                } else {
                    auto r = (*f)();
                    std::lock_guard lock(w->mutex);
                    w->value.emplace(std::move(r));
                }
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
        std::unique_lock lock(w->mutex);
        const auto finished = [&] { return w->phase == Phase::done; };
        if (deadline && !w->cv.wait_until(lock, *deadline, finished) && w->phase == Phase::pending) {
            w->phase = Phase::abandoned;
            lock.unlock();
            loop_busy();
        }
        w->cv.wait(lock, finished);
        if (w->error) w->error.rethrow();
        if constexpr (std::is_reference_v<R>) {
            return static_cast<R>(**w->value);
        } else if constexpr (!std::is_void_v<R>) {
            return std::move(*w->value);
        }
    }
    /// Counts the timeout and throws 503 "main loop busy".
    [[noreturn]] void loop_busy() const;
    void background_failed(const char* what);

    struct Hooks {
        std::vector<Hook> start, stop, reload;
        std::vector<std::function<void(Application&, http::Server&)>> http;
        std::vector<std::function<void(Application&, Console&)>> console;
        std::vector<std::function<void(Application&, web::Dashboard&)>> web;
    };

    // The internal services (src/app/services.hpp) reach the hooks, the
    // components and the virtual setup functions.
    friend class detail::Messaging;
    friend class detail::RemoteConsole;
    friend class detail::HttpFrontend;

    int parse_command_line(int argc, char** argv);
    Config load_config() const;
    void declare_framework_config(config::Schema& schema);
    bool validate_config(const Config& config, bool at_start);
    void attach(std::unique_ptr<Component> component);
    void start_observability(const Config& config);
    void setup_logging(const Config& config);
    void start_signal_thread();
    void stop_signal_thread();
    /// Run `blocking` on a helper thread while this (main) thread keeps
    /// executing posted tasks, so it cannot deadlock on invoke()/call().
    void drain_while(const std::function<void()>& blocking);
    void main_loop();
    void do_reload();
    void shutdown();
    /// Milliseconds since the main loop's last heartbeat (0 before it runs).
    std::int64_t loop_age_ms() const;

    AppInfo info_;
    Hooks hooks_;
    std::unique_ptr<CLI::App> cli_;

    // command line state
    std::string config_file_;
    std::vector<std::string> overrides_;
    std::string log_level_;
    std::string log_file_;
    int verbosity_ = 0;
    bool syslog_ = false;
    bool interactive_ = false;
    std::string http_listen_;
    std::string instance_;
    bool manual_clock_ = false;
    std::atomic<int> slow_task_ms_{200};  ///< [forge].slow_task_ms
    bool print_config_ = false;
    bool help_config_ = false;
    bool check_config_ = false;

    mutable std::mutex config_mutex_;
    Config config_;

    Logger logger_;
    EventBus events_;
    Alarms alarms_;
    Channel<Task> tasks_{256};
    std::unique_ptr<ThreadPool> pool_;
    std::unique_ptr<Scheduler> scheduler_;
    std::unique_ptr<web::Dashboard> dashboard_;
    std::atomic<std::thread::id> main_thread_{};

    config::Schema schema_;
    Audit audit_;
    Health health_;
    Auth auth_;
    std::unique_ptr<ControlService> control_;
    std::vector<std::unique_ptr<Component>> components_;
    // services (src/app/)
    std::unique_ptr<detail::StateStore> state_;
    std::unique_ptr<detail::Watchdog> watchdog_;
    std::unique_ptr<detail::HubRegistration> hub_;
    std::unique_ptr<detail::Messaging> messaging_;
    std::unique_ptr<detail::RemoteConsole> console_;
    std::unique_ptr<detail::HttpFrontend> http_;
    bool hub_registration_ = true;
    std::atomic<int> stop_signal_{0};                 ///< signal that asked us to stop (0 = none)
    std::atomic<int> request_timeout_ms_{2000};       ///< [forge].request_timeout_ms
    std::atomic<std::int64_t> loop_tick_ms_{0};       ///< last main loop heartbeat (steady clock)
    std::atomic<bool> state_event_pending_{false};

    std::atomic<bool> stop_requested_{false};
    std::atomic<int> exit_code_{0};
    std::atomic<bool> signal_thread_running_{false};
    std::thread signal_thread_;
    std::chrono::steady_clock::time_point started_at_;
};

}  // namespace forge

/// Define main() for an Application subclass.
#define FORGE_MAIN(AppClass)                 \
    int main(int argc, char** argv) {        \
        AppClass app;                        \
        return app.run(argc, argv);          \
    }
