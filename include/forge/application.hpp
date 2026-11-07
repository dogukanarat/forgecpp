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
//   7. optional interactive console (-i) with builtin commands, as a backup
//      to the web UI (same controls: `controls`, `set`, `press`)
//   8. signal handling: SIGINT/SIGTERM stop, SIGHUP reloads the config
//   9. systemd notify + watchdog support (Type=notify units)
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
#include "forge/channel.hpp"
#include "forge/config.hpp"
#include "forge/events.hpp"
#include "forge/json.hpp"
#include "forge/log.hpp"
#include "forge/scheduler.hpp"
#include "forge/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
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

namespace http {
class Server;
class EventStream;
}  // namespace http
namespace web {
class Dashboard;
}
class Console;

struct AppInfo {
    AppInfo(std::string name_ = "app", std::string version_ = "0.1.0", std::string description_ = {},
            std::vector<std::filesystem::path> config_paths_ = {})
        : name(std::move(name_)),
          version(std::move(version_)),
          description(std::move(description_)),
          config_paths(std::move(config_paths_)) {}

    std::string name;                  ///< used for logger, config file and syslog ident
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
    template <class F>
    auto invoke(F&& fn) -> std::invoke_result_t<std::decay_t<F>> {
        if (on_main_loop()) return fn();
        return call(std::forward<F>(fn)).get();
    }
    /// True on the thread running the main loop.
    bool on_main_loop() const { return std::this_thread::get_id() == main_thread_.load(); }

    /// Run `fn` on the main loop and get its result as a future.
    /// Never wait on the future from the main loop itself (use invoke()).
    template <class F>
    auto call(F&& fn) -> std::future<std::invoke_result_t<std::decay_t<F>>> {
        using R = std::invoke_result_t<std::decay_t<F>>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        auto future = task->get_future();
        post([task] { (*task)(); });
        return future;
    }

    /// Timers; callbacks run on the main loop.
    Scheduler::Id after(Duration delay, Task task);
    Scheduler::Id every(Duration period, Task task, bool run_immediately = false);
    bool cancel(Scheduler::Id id);

    /// Publish an event asynchronously on the main loop.
    template <class Event>
    void emit(Event event) {
        post([this, e = std::move(event)] { events_.publish(e); });
    }

    // --- services ---------------------------------------------------------
    const AppInfo& info() const { return info_; }
    /// Snapshot of the effective configuration (file + overrides + flags).
    Config config() const;
    Logger logger() const;
    /// Valid from on_start() until run() returns.
    ThreadPool& pool();
    EventBus& events() { return events_; }
    /// Valid from on_start() until run() returns.
    Scheduler& scheduler();
    /// nullptr unless the HTTP server is enabled.
    http::Server* http() { return http_.get(); }
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
    /// nullptr unless running interactively (-i).
    Console* console() { return console_.get(); }
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
    struct Hooks {
        std::vector<Hook> start, stop, reload;
        std::vector<std::function<void(Application&, http::Server&)>> http;
        std::vector<std::function<void(Application&, Console&)>> console;
        std::vector<std::function<void(Application&, web::Dashboard&)>> web;
    };

    int parse_command_line(int argc, char** argv);
    Config load_config() const;
    void setup_logging(const Config& config);
    void start_signal_thread();
    void stop_signal_thread();
    void create_http(const Config& config);
    void create_web(const Config& config);
    void register_web_console(Console& console);
    void push_web_state();
    /// Run `blocking` on a helper thread while this (main) thread keeps
    /// executing posted tasks, so it cannot deadlock on invoke()/call().
    void drain_while(const std::function<void()>& blocking);
    void create_console(const Config& config);
    void register_console_builtins(Console& console);
    void main_loop();
    void do_reload();
    void shutdown();

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
    bool print_config_ = false;

    mutable std::mutex config_mutex_;
    Config config_;

    Logger logger_;
    EventBus events_;
    Alarms alarms_;
    std::atomic<bool> alarms_dirty_{true};
    Channel<Task> tasks_{256};
    std::unique_ptr<ThreadPool> pool_;
    std::unique_ptr<Scheduler> scheduler_;
    std::unique_ptr<http::Server> http_;
    std::unique_ptr<web::Dashboard> dashboard_;
    std::shared_ptr<http::EventStream> web_events_;
    spdlog::sink_ptr web_log_sink_;
    std::unique_ptr<Console> console_;
    std::atomic<std::thread::id> main_thread_{};

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
