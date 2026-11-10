#pragma once
// Internal services of forge::Application (not installed). Application owns
// one of each and keeps the lifecycle and the public API; each service owns
// the state of one concern:
//
//   StateStore       persistent controls ([state].file)
//   LayoutStore      card layouts of the web pages ([web].layout_file)
//   Watchdog         systemd watchdog pings, paused when a critical component failed
//   HubRegistration  registration with forge-hub ([hub])
//   Messaging        ZeroMQ: events, streams and the command endpoint ([forge])
//   RemoteConsole    command registry, local REPL and telnet ([console])
//   HttpFrontend     HTTP server, framework routes, web events and logs ([http], [web])

#include "forge/application.hpp"
#include "forge/thread_pool.hpp"

#include "../common.hpp"
#include "forge/console.hpp"
#include "forge/http.hpp"
#include "forge/hub.hpp"
#include "forge/serial.hpp"
#include "forge/telnet.hpp"
#include "forge/zmq.hpp"

#include "forge/hub.forge.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace forge::detail {

class WebLog;

bool is_loopback(const std::string& host);
/// Caller of a console command: the local terminal (admin) or a telnet session.
Caller console_caller();

/// The addresses the program listens on, by name ("control", "http", ...).
/// The main loop writes one as each opens; any thread reads them (the "info"
/// command is answered on the links' own threads, also during the start).
class Endpoints {
public:
    void set(const std::string& name, std::string endpoint) {
        std::lock_guard lock(mutex_);
        endpoints_[name] = std::move(endpoint);
    }
    void clear() {
        std::lock_guard lock(mutex_);
        endpoints_.clear();
    }
    std::map<std::string, std::string> all() const {
        std::lock_guard lock(mutex_);
        return endpoints_;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::string> endpoints_;
};

/// Persistent controls: restored at start-up, saved when they changed.
class StateStore {
public:
    /// Restore the persistent controls of `dashboard` from `file` ("" = off).
    void restore(web::Dashboard& dashboard, const std::string& file, const Logger& logger);
    void mark_dirty() { dirty_ = true; }
    /// Save if something changed (`force`: also when nothing did). The values
    /// are read on the calling thread (the main loop); with `pool` the file is
    /// written on a pool thread (fsync on flash takes long), else here. A
    /// write that is still running is not started again: the next call does.
    void save(web::Dashboard& dashboard, const Logger& logger, bool force = false, ThreadPool* pool = nullptr);
    const std::string& file() const { return file_; }

private:
    void write(std::uint64_t generation, const json& values, const Logger& logger);

    std::string file_;
    std::atomic<bool> dirty_{false};
    std::atomic<bool> writing_{false};
    std::uint64_t generation_ = 0;  // of the snapshots, counted on the main loop
    std::mutex write_mutex_;        // one write at a time (the pool's, or the forced one at shutdown)
    std::uint64_t written_ = 0;     // the newest snapshot in the file (write_mutex_)
};

/// Card layouts of the web pages (forge.layout in forge.js). Thread-safe.
/// Each page has the device's layout and, for signed-in users ([auth]), their
/// own: a user sees theirs when they saved one, else the device's.
class LayoutStore {
public:
    /// $STATE_DIRECTORY/web-layout.json, else ~/.local/state/<name>/web-layout.json.
    static std::string default_file(const std::string& app_name);
    static bool valid_page(const std::string& page);

    /// `file`: "" = default_file(app_name), "-" = not stored (kept in memory).
    void load(std::string file, const std::string& app_name, const Logger& logger);
    /// The user's layout of `page` if they have one, else the device's (null: none).
    json get(const std::string& page, const std::string& user = {}) const;
    /// The user's layout ("" = the device's). Returns whether it was stored in a file.
    bool put(const std::string& page, json layout, const std::string& user = {});
    /// Remove the user's layout (they see the device's again), or the device's ("").
    void remove(const std::string& page, const std::string& user = {});
    const std::string& file() const { return file_; }

private:
    void save();  // with mutex_ held

    mutable std::mutex mutex_;
    json layouts_ = json::object();
    std::string file_;
    Logger logger_;
};

/// systemd watchdog: pings while healthy; stops pinging when a critical
/// component has failed longer than [health].watchdog_grace_s, so systemd
/// restarts the application.
class Watchdog {
public:
    /// True when the ping should be sent.
    bool tick(Health& health, double grace_s, const Logger& logger);
    bool paused() const { return paused_; }

private:
    bool paused_ = false;
};

/// Registration with forge-hub (in the background; never blocks start-up).
class HubRegistration {
public:
    explicit HubRegistration(Application& app) : app_(app) {}
    void start(const Config& cfg);
    /// Tell the hub why the application ends.
    void stop(forge_hub_unregister_reason reason, int exit_code);
    void reset() { client_.reset(); }

private:
    Application& app_;
    std::unique_ptr<hub::Client> client_;
};

/// ZeroMQ: [forge].events, [forge].data (streams), [forge].control (commands),
/// and the c2 links: [serial] (a tty) and [can] (an ISO-TP channel). A link is
/// opened again when it goes away (an unplugged USB adapter, an interface
/// that went down) and reported under health by its name ("serial", "can").
class Messaging {
public:
    Messaging(Application& app, Endpoints& endpoints) : app_(app), endpoints_(endpoints) {}
    /// Events and streams (before on_start(): it may publish).
    void start(const Config& cfg);
    /// The command endpoint and the links (after on_start(): everything is ready).
    void start_control(const Config& cfg);
    /// Stop the command endpoint (may wait for commands on the main loop).
    void stop_control();
    void stop();
    void reset();

    zmq::Stream& stream(const std::string& name, const pb::Type& type, const zmq::StreamOptions& options);
    zmq::Stream* find_stream(const std::string& name);
    std::vector<zmq::StreamInfo> stream_list() const;
    /// "state" events for the values that changed since the last call (main loop).
    void publish_state(const json& current);
    bool wants_state() const;

private:
    template <pb::Message T>
    void publish_event(const char* topic, const T& body);
    /// A c2 link the application's commands are answered on.
    struct LinkSlot {
        std::string name;   ///< "serial", "can": health component, metric label
        std::string where;  ///< "/dev/ttyUSB0", "can0 0x701/0x700"
        std::function<serial::LinkOptions()> options;  ///< for each attempt to open it
        std::unique_ptr<serial::Link> link;            ///< main loop
        std::string error;                             ///< the last reason it could not be opened
        std::mutex stats_mutex;
        serial::Link::Stats stats;  ///< copied every second for the metrics (any thread reads them)
    };
    void add_link(const std::string& name, const std::string& where, std::function<serial::LinkOptions()> options);
    /// Open a link (main loop); false and its health component failed when it cannot be.
    bool open_link(LinkSlot& l);
    /// Every second: notice a link that went away, open it again.
    void check_links();

    Application& app_;
    Endpoints& endpoints_;
    std::unique_ptr<zmq::Publisher> events_pub_;
    mutable std::mutex data_mutex_;
    std::unique_ptr<zmq::Publisher> data_pub_;
    std::unique_ptr<zmq::Server> control_server_;
    std::vector<std::unique_ptr<LinkSlot>> links_;
    bool links_stopped_ = false;
    std::vector<Subscription> subscriptions_;
    json last_state_ = json::object();
};

/// The built-in commands (commands.cpp), and the conversions the events share.
void register_builtin_commands(Application& app, Application::Impl& d);
void fill(forge_alarm& out, const Alarm& a);
void fill(forge_log_record& out, const log::Record& r);
void fill(forge_audit_record& out, const Audit::Record& r);
void fill(forge_health_reply& out, const Health::Report& h, std::size_t from = 0);

/// Console commands (one registry for -i and telnet), the local REPL, telnet.
class RemoteConsole {
public:
    RemoteConsole(Application& app, Application::Impl& d) : app_(app), d_(d) {}
    void create(const Config& cfg);
    void start_telnet(const Config& cfg);
    void start_repl() {
        if (console_) console_->start();
    }
    void stop();
    void reset();
    Console* console() { return console_.get(); }

private:
    void register_builtins(Console& c);
    void register_framework(Console& c);
    void register_controls(Console& c);

    Application& app_;
    Application::Impl& d_;  // the application's state (impl.hpp)
    std::unique_ptr<Console> console_;
    std::unique_ptr<Telnet> telnet_;
};

/// The HTTP server with the framework's routes (/health, /metrics,
/// /forge/api/...), the built-in pages, live events and logs for the browser.
class HttpFrontend {
public:
    HttpFrontend(Application& app, Application::Impl& d) : app_(app), d_(d) {}
    /// Create the server and its routes ([http].enabled), not listening yet.
    void create(const Config& cfg);
    /// Listen; start the live updates.
    void start(const Config& cfg);
    void stop();
    void reset();

    http::Server* server() { return http_.get(); }
    void publish(const std::string& event, const json& data);
    /// Changed controls and alarms to the open pages (main loop).
    void push_state();

private:
    void create_web(const Config& cfg);
    /// The live messages of a stream for the panel (created on demand).
    std::shared_ptr<http::EventStream> watch_stream(const std::string& name);
    void drop_unwatched_streams();
    static std::size_t pages_signature(const std::string& root);

    struct StreamWatch {
        std::shared_ptr<http::EventStream> events;
        Subscription subscription;
        std::chrono::steady_clock::time_point since;
    };

    Application& app_;
    Application::Impl& d_;  // the application's state (impl.hpp)
    std::mutex watch_mutex_;
    std::map<std::string, StreamWatch> watches_;
    std::unique_ptr<http::Server> http_;
    std::shared_ptr<http::EventStream> events_;
    std::shared_ptr<WebLog> weblog_;  // log lines for the dashboard (web.logs)
    std::vector<Subscription> subscriptions_;
    std::atomic<bool> alarms_dirty_{true};
    LayoutStore layouts_;
    std::string pages_root_;  // [web].root with live_reload
    std::size_t pages_signature_ = 0;
    int stall_ms_ = 5000;  // [health].stall_ms
};

}  // namespace forge::detail
