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
//  10. ZeroMQ: commands ([forge].control: c2 frames, forgectl and other
//      programs), events ([forge].events: state, alarms, logs, audit, health,
//      metrics) and the application's streams ([forge].data)
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
#include "forge/component.hpp"
#include "forge/config.hpp"
#include "forge/config_schema.hpp"
#include "forge/events.hpp"
#include "forge/health.hpp"
#include "forge/json.hpp"
#include "forge/log.hpp"
#include "forge/metrics.hpp"
#include "forge/pb.hpp"
#include "forge/scheduler.hpp"
#include "forge/tmtc.hpp"
#include "forge/worker.hpp"

#include <csignal>

#include <atomic>
#include <chrono>
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
}  // namespace CLI

namespace forge {

class ThreadPool;  // forge/thread_pool.hpp (BS::thread_pool), parsed only where the pool is used

namespace http {
class Server;
class EventStream;
}  // namespace http
namespace web {
class Dashboard;
}  // namespace web
using Controls = web::Dashboard;  // also in forge/web.hpp
namespace zmq {
class Publisher;
class Stream;
struct StreamInfo;
struct StreamOptions;
}  // namespace zmq
class Console;
class ControlService;
class Telnet;
namespace hub {
class Client;
}  // namespace hub

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
    explicit Application(AppInfo info, const std::string& installed_config = FORGE_APP_INSTALLED_CONFIG);
    virtual ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /// Run the whole lifecycle and return the process exit code.
    int run(int argc, char** argv);

    /// Request shutdown (thread-safe, also from signal context via the signal thread).
    void stop(int exit_code = 0);
    bool stopping() const;

    /// Re-read the configuration file and call on_reload() (thread-safe).
    void reload();

    // --- main loop --------------------------------------------------------
    /// Run `task` on the main loop thread (thread-safe). False when the loop's
    /// queue is full ([loop].max_pending_tasks: the loop is stuck or far
    /// behind): the task is dropped and counted (forge.loop.dropped).
    bool post(Task task);

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
    /// (commands, HTTP) use it with request_timeout().
    template <class F>
    auto invoke_for(std::chrono::milliseconds timeout, F&& fn) -> std::invoke_result_t<std::decay_t<F>> {
        return invoke_until(std::chrono::steady_clock::now() + timeout, std::forward<F>(fn));
    }

    /// [loop].request_timeout_ms: how long a request waits for the main loop.
    std::chrono::milliseconds request_timeout() const;

    /// True on the thread running the main loop.
    bool on_main_loop() const;

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
        } else if (!post([task] { (*task)(); })) {
            loop_busy();  // the queue is full: 503, like invoke_for() on a stuck loop
        }
        return future;
    }

    /// Heavy work off the main loop: `work` runs on the worker pool, then
    /// `then(result)` on the main loop (where the application's state may be
    /// touched). If `work` throws, `on_error(message)` runs on the main loop
    /// instead (to show it, retry, raise an alarm); without it the error is
    /// logged.
    ///
    ///   background([path] { return load_big_file(path); },
    ///              [this](Data d) { data_ = std::move(d); },
    ///              [this](const std::string& why) { alarms().raise("load", Severity::warning, why); });
    template <class Work, class Then>
    void background(Work work, Then then) {
        background(std::move(work), std::move(then),
                   [this](const std::string& why) { background_failed(why.c_str()); });
    }
    template <class Work, class Then, class OnError>
    void background(Work work, Then then, OnError on_error) {
        using R = std::invoke_result_t<Work>;
        run_in_pool([this, work = std::move(work), then = std::move(then), on_error = std::move(on_error)]() mutable {
            std::string error;
            try {
                if constexpr (std::is_void_v<R>) {
                    work();
                    post([then = std::move(then)]() mutable { then(); });
                } else {
                    auto result = std::make_shared<R>(work());
                    post([then = std::move(then), result]() mutable { then(std::move(*result)); });
                }
                return;
            } catch (const std::exception& e) {
                error = e.what();
            } catch (...) {
                error = "unknown exception";
            }
            post([on_error = std::move(on_error), error = std::move(error)]() mutable { on_error(error); });
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
    void use_manual_clock();
    bool manual_clock() const;
    /// Manual clock only, not from the main loop: move time forward by `d`.
    /// Every timer that falls due on the way runs on the main loop, in order,
    /// seeing now() == its due time, before the next one; returns how many ran.
    std::size_t advance_time(Duration d);

    /// Publish an event asynchronously on the main loop.
    template <class Event>
    void emit(Event event) {
        post([this, e = std::move(event)] { events().publish(e); });
    }

    // --- services ---------------------------------------------------------
    const AppInfo& info() const;
    /// Instance name when the application runs several times on a device
    /// (--instance, or the instance of its systemd template unit <name>@<instance>);
    /// "" otherwise. Valid from run() on.
    const std::string& instance() const;
    /// "<name>" or "<name>@<instance>": how forge-hub, the logs and /forge/api/info name it.
    std::string id() const;
    /// Snapshot of the effective configuration (file + overrides + flags).
    Config config() const;
    Logger logger() const;
    /// Valid from on_start() until run() returns.
    ThreadPool& pool();
    EventBus& events();
    /// Valid from on_start() until run() returns.
    Scheduler& scheduler();
    /// nullptr unless the HTTP server is enabled.
    http::Server* http();
    /// Control registry declared in setup_web(); valid from on_start() until run()
    /// returns, with or without HTTP. Served at /forge/ when the web UI is enabled,
    /// and by the console and the built-in commands.
    web::Dashboard* dashboard();
    /// Alarm list shown in the web UI (bell, banner, panel) and the console
    /// (`alarms`, `ack`). Thread-safe.
    Alarms& alarms();
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
    Audit& audit();
    /// Component health; drives /health, the watchdog and the "health" events.
    Health& health();
    const Auth& auth() const;
    /// Process-wide metrics (forge::metrics::registry()).
    metrics::Registry& metrics() { return metrics::registry(); }

    /// A stream of `T` messages on [forge].data (created on first use;
    /// thread-safe publish). See forge/zmq.hpp.
    template <pb::Message T>
    zmq::Stream& stream(const std::string& name) {
        return stream(name, *pb::type_of<T>);
    }
    template <pb::Message T>
    zmq::Stream& stream(const std::string& name, const zmq::StreamOptions& options) {
        return stream(name, *pb::type_of<T>, options);
    }
    zmq::Stream& stream(const std::string& name, const pb::Type& type);
    zmq::Stream& stream(const std::string& name, const pb::Type& type, const zmq::StreamOptions& options);

    /// The commands this program answers, by number (links) and by name (web).
    /// The built-in ones (proto/forge/messages.proto) use 0x0001..0x00FF.
    tmtc::Registry& commands();
    /// A command (docs/commands.md). Its handler runs on the main loop, like
    /// the controls; a busy loop refuses it with EAGAIN after
    /// [loop].request_timeout_ms. The caller needs `role`: the default,
    /// operator, is for a command that changes something, and those are
    /// written to the audit trail with their request; Role::viewer is for a
    /// query (no check, no audit record).
    /// Add commands in the constructor or setup_web(): the links read the
    /// registry from their own threads once they run.
    template <pb::Message Req, pb::Message Rsp>
    void command(std::uint16_t id, std::string name, tmtc::Handler<Req, Rsp> handler, Role role = Role::operator_) {
        if (commands().closed()) throw std::logic_error("command '" + name + "': add commands before the start");
        commands().add<Req, Rsp>(id, name, [this, name, role, h = std::move(handler)](const Req& req, Rsp& rsp) {
            return run_command(name, role, [&] { return pb::to_json(req); },
                               [&] { return invoke_for(request_timeout(), [&] { return h(req, rsp); }); });
        });
    }
    /// Endpoints this application serves (control, events, data, http, telnet).
    std::map<std::string, std::string> endpoints() const;

    /// Apply [threads.<name>] (policy, priority, cpus) to the calling thread and
    /// name it for the trace view. Returns "" or what could not be applied.
    std::string configure_thread(const std::string& name);

    /// A thread of the application (forge/worker.hpp), from on_start() on:
    /// it applies [threads.<name>], and an exception that ends it is logged
    /// and raises the alarm "worker.<name>". Stopped and joined before
    /// on_stop(), so on_stop() may release what it used.
    ///
    ///   worker("producer", [this](std::stop_token stop) {
    ///       while (!stop.stop_requested()) { produce(); forge::sleep_for(stop, 50ms); }
    ///   });
    Worker& worker(const std::string& name, Worker::Body body);

    /// Add a component (before run()); see component.hpp.
    template <class T, class... A>
    T& add_component(A&&... args) {
        auto c = std::make_unique<T>(std::forward<A>(args)...);
        T& ref = *c;
        attach(std::move(c));
        return ref;
    }
    /// Command line parser; add your own options in the constructor or setup_cli().
    CLI::App& cli();
    std::chrono::steady_clock::duration uptime() const;

    // --- lambda hooks (alternative to overriding the virtuals) ------------
    Application& hook_start(Hook hook);
    Application& hook_stop(Hook hook);
    Application& hook_reload(Hook hook);
    /// Declare configuration keys (the lambda form of setup_config()).
    Application& hook_config(std::function<void(Application&, config::Schema&)> hook);
    /// Add command line options (the lambda form of setup_cli()).
    Application& hook_cli(std::function<void(Application&, CLI::App&)> hook);
    Application& hook_http(std::function<void(Application&, http::Server&)> hook);
    Application& hook_console(std::function<void(Application&, Console&)> hook);
    /// Declare controls and actions (the lambda form of setup_controls()).
    Application& hook_controls(std::function<void(Application&, Controls&)> hook);
    /// The old name of hook_controls().
    Application& hook_web(std::function<void(Application&, web::Dashboard&)> hook);

protected:
    /// Add command line options (called before parsing).
    virtual void setup_cli(CLI::App& cli);
    /// Declare configuration keys (types, ranges, docs); see config_schema.hpp.
    virtual void setup_config(config::Schema& schema);
    /// Applications that must not register with forge-hub (the hub itself).
    void disable_hub_registration();
    /// Register HTTP routes (called before the server starts listening).
    virtual void setup_http(http::Server& server);
    /// Register console commands.
    virtual void setup_console(Console& console);
    /// Declare the controls and actions (groups, buttons, switches, sliders,
    /// values, links): one registry for the web panel, forgectl, the console,
    /// telnet and the links. Order at start-up: setup_config() -> command line
    /// -> setup_controls() -> the persisted values ([state].file) are restored
    /// -> on_start(). A control's setter must therefore work before on_start():
    /// read the configuration here (config()) when a value depends on it.
    virtual void setup_controls(Controls& controls);
    /// The old name of setup_controls(); both are called.
    virtual void setup_web(web::Dashboard& dashboard);
    /// Called on the main loop once everything is set up. Throw to abort startup.
    virtual void on_start();
    /// Called on the main loop during shutdown.
    virtual void on_stop();
    /// Called on the main loop after the configuration was reloaded.
    virtual void on_reload(const Config& config);

private:
    template <class F>
    auto invoke_until(std::optional<std::chrono::steady_clock::time_point> deadline,
                      F&& fn) -> std::invoke_result_t<std::decay_t<F>> {
        using R = std::invoke_result_t<std::decay_t<F>>;
        if (on_main_loop()) return fn();
        // invoke_erased() runs `run` on the loop and waits for it; once started
        // it is always waited for, so `fn` and the result may live here.
        if constexpr (std::is_void_v<R>) {
            invoke_erased(deadline, [&fn] { fn(); });
        } else if constexpr (std::is_reference_v<R>) {
            std::remove_reference_t<R>* result = nullptr;
            invoke_erased(deadline, [&fn, &result] { result = &fn(); });
            return static_cast<R>(*result);
        } else {
            std::optional<R> result;
            invoke_erased(deadline, [&fn, &result] { result.emplace(fn()); });
            return std::move(*result);
        }
    }
    /// Post `run` to the main loop and wait as invoke() / invoke_for()
    /// describe (abandon before it started at `deadline`; exceptions are
    /// described on the loop and thrown anew here).
    void invoke_erased(std::optional<std::chrono::steady_clock::time_point> deadline, const std::function<void()>& run);
    /// Run `fn` on the worker pool (the non-template part of background()).
    void run_in_pool(std::function<void()> fn);
    /// Counts the timeout and throws 503 "main loop busy".
    [[noreturn]] void loop_busy() const;
    void background_failed(const char* what);
    /// The role check and the audit record around a command's handler.
    Status run_command(const std::string& name, Role role, const std::function<json()>& request,
                       const std::function<Status()>& run);
    void attach(std::unique_ptr<Component> component);

public:
    /// The application's state and internal services: defined inside the
    /// library (src/app/impl.hpp), opaque to applications. Nothing of the
    /// layout of an Application is visible here, so it can change without
    /// rebuilding the applications.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace forge

/// Define main() for an Application subclass.
#define FORGE_MAIN(AppClass)          \
    int main(int argc, char** argv) { \
        AppClass app;                 \
        return app.run(argc, argv);   \
    }
