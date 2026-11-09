#pragma once
// forge::Application::Impl: the state of an application and the services it
// is made of (not installed). forge::Application is the public face; its
// methods forward here, and the lifecycle (prepare, start, shutdown) lives
// here, so nothing of this is in forge/application.hpp.

#include "forge/application.hpp"
#include "forge/console.hpp"
#include "forge/control.hpp"
#include "forge/thread_pool.hpp"
#include "forge/web.hpp"

#include "main_loop.hpp"
#include "services.hpp"
#include "signals.hpp"

#include <CLI/CLI.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace forge {

namespace detail {

/// What the command line set (the framework's own options).
struct CommandLine {
    std::string config_file;
    std::vector<std::string> overrides;
    std::string log_level, log_file, http_listen;
    int verbosity = 0;
    bool syslog = false, interactive = false;
    bool print_config = false, help_config = false, check_config = false;
};

/// The protected hooks of a Component, for the application that owns it.
struct ComponentAccess {
    static void attach(Component& c, Application& app) {
        c.app_ = &app;
        c.logger_ = log::get(c.name());
    }
    static void setup_config(Component& c, config::Schema& s) { c.setup_config(s); }
    static void setup_controls(Component& c, Controls& ui) {
        c.setup_controls(ui);
        c.setup_web(ui);  // the old name
    }
    static void setup_console(Component& c, Console& console) { c.setup_console(console); }
    static void setup_http(Component& c, http::Server& server) { c.setup_http(server); }
    static void on_start(Component& c) { c.on_start(); }
    static void on_stop(Component& c) { c.on_stop(); }
    static void on_reload(Component& c, const Config& cfg) { c.on_reload(cfg); }
};

}  // namespace detail

struct Application::Impl {
    Impl(Application& app, AppInfo info, const std::string& installed_config);
    ~Impl();

    // --- lifecycle (application.cpp) -----------------------------------------------
    int run(int argc, char** argv);
    /// The command line, the schema and the configuration; logging. An exit
    /// code when the run ends here (--help-config, --check-config, an error).
    std::optional<int> prepare(int argc, char** argv);
    /// Everything up, in order; returns once the application is ready.
    void start();
    void shutdown();
    /// Read the configuration again and apply it (main loop).
    void reload();
    void on_signal(int sig);
    /// After a control changed: persistence, live updates, "state" events.
    void control_changed(bool persistent);
    Config snapshot() const;

    // --- the hooks of the application, its components and the lambdas, in that order ---
    void setup_console(Console& console);
    void setup_http(http::Server& server);

    // --- configuration (config.cpp) --------------------------------------------------
    int parse_command_line(int argc, char** argv);
    Config load_config() const;
    void declare_framework_config();
    bool validate_config(const Config& cfg, bool at_start);

    // --- observability.cpp -----------------------------------------------------------
    void start_observability(const Config& cfg);

    // --- state -----------------------------------------------------------------------
    struct Hooks {
        std::vector<Hook> start, stop, reload;
        std::vector<std::function<void(Application&, config::Schema&)>> config;
        std::vector<std::function<void(Application&, CLI::App&)>> cli;
        std::vector<std::function<void(Application&, http::Server&)>> http;
        std::vector<std::function<void(Application&, Console&)>> console;
        std::vector<std::function<void(Application&, Controls&)>> controls;
    };

    Application& app;
    AppInfo info;
    std::string instance;
    Hooks hooks;
    CLI::App cli;
    detail::CommandLine options;
    bool manual_clock = false;
    bool hub_registration = true;

    mutable std::mutex config_mutex;
    Config config;
    config::Schema schema;

    Logger logger;
    detail::MainLoop loop;
    detail::SignalWatcher signals;
    std::unique_ptr<ThreadPool> pool;
    std::unique_ptr<Scheduler> scheduler;

    EventBus events;
    Alarms alarms;
    Audit audit;
    Health health;
    Auth auth;
    tmtc::Registry commands;
    std::unique_ptr<Controls> controls;
    std::unique_ptr<ControlService> control;
    std::vector<std::unique_ptr<Component>> components;
    std::mutex workers_mutex;
    std::vector<std::unique_ptr<Worker>> workers;

    detail::Endpoints endpoints;  ///< declared before the services that write to it
    detail::StateStore state;
    detail::Watchdog watchdog;
    detail::HubRegistration hub;
    detail::Messaging messaging;
    detail::RemoteConsole console;
    detail::HttpFrontend http;

    std::atomic<bool> stop_requested{false};
    std::atomic<int> stop_signal{0};  ///< the signal that asked us to stop (0: none)
    std::atomic<int> exit_code{0};
    std::atomic<double> watchdog_grace_s{0};  ///< [health].watchdog_grace_s
    std::atomic<bool> state_event_pending{false};
    std::chrono::steady_clock::time_point started_at;

private:
    void setup_logging(const Config& cfg);
    /// The settings a reload applies again: the main loop's, the watchdog grace time.
    void apply_settings(const Config& cfg);
    /// Pool, timers, signals, access control, observability, the control service.
    void start_core(const Config& cfg);
    /// The control registry of the application, its components and its hooks; the saved values.
    void declare_controls(const Config& cfg);
    /// Watchdog pings, the loop's heartbeat, saving the persistent controls.
    void start_housekeeping();
};

}  // namespace forge
