// Application: the command line, the configuration file and the keys the
// framework declares ([log], [http], [forge], ...), and their validation.
#include "impl.hpp"

#include "forge/version.hpp"

#include <CLI/CLI.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace forge {

int Application::Impl::parse_command_line(int argc, char** argv) {
    app.setup_cli(cli);
    for (auto& h : hooks.cli) h(app, cli);  // hook_cli()
    try {
        cli.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return cli.exit(e);
    }
    if (instance.empty()) {
        if (const char* env = std::getenv("FORGE_INSTANCE"); env && *env) {
            instance = env;
        } else {
            instance = hub::instance_of(hub::unit_of(::getpid()));  // radio-controller@rx2.service
        }
    }
    if (instance.size() > 32 ||
        instance.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") !=
            std::string::npos) {
        std::cerr << info.name << ": invalid instance name '" << instance << "'\n";
        return 2;
    }
    return -1;
}

Config Application::Impl::load_config() const {
    Config cfg;
    if (!options.config_file.empty()) {
        cfg = Config::load_file(options.config_file);
    } else {
        for (const auto& candidate : info.config_paths) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec)) {
                cfg = Config::load_file(candidate);
                break;
            }
        }
    }

    for (const auto& o : options.overrides) cfg.apply_override(o);

    // Explicit flags win over the file and -D overrides.
    if (options.verbosity == 1) cfg.set("log.level", "debug");
    if (options.verbosity >= 2) cfg.set("log.level", "trace");
    if (!options.log_level.empty()) cfg.set("log.level", options.log_level);
    if (!options.log_file.empty()) cfg.set("log.file", options.log_file);
    if (options.syslog) cfg.set("log.syslog", true);
    if (options.interactive) cfg.set("console.enabled", true);
    if (!options.http_listen.empty()) {
        cfg.set("http.enabled", true);
        const auto colon = options.http_listen.rfind(':');
        const std::string port =
            colon == std::string::npos ? options.http_listen : options.http_listen.substr(colon + 1);
        if (colon != std::string::npos && colon > 0) cfg.set("http.host", options.http_listen.substr(0, colon));
        try {
            cfg.set("http.port", static_cast<std::int64_t>(std::stoi(port)));
        } catch (const std::exception&) {
            throw ConfigError("invalid --http value '" + options.http_listen + "'");
        }
    }
    return cfg;
}

void Application::Impl::declare_framework_config() {
    auto& s = schema;
    s.section("log", "Logging");
    s.string("log.level", "info", "Default level")
        .choices({"trace", "debug", "info", "warn", "error", "critical", "off"});
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
    s.integer("http.threads", 12, "Worker threads (at least [web].max_clients + 4: every open page holds one)")
        .range(2, 256);
    s.open_section("http.auth", "Older single-user credentials (prefer [auth])");

    s.section("web", "Control panel");
    s.boolean("web.enabled", true, "Serve the control panel at /forge/");
    s.string("web.title", info.name, "Title");
    s.string("web.root", "", "Serve pages from this directory instead of the embedded ones");
    s.boolean("web.live_reload", true, "With web.root: reload the open pages when a file there changes");
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
    s.string("console.prompt", info.name + "> ", "Prompt");
    s.string("console.history", "", "History file (default ~/.<app>_history)");
    s.boolean("console.color", true, "Colors (default: on a terminal)").computed();
    s.string("console.telnet", "", "Telnet console HOST:PORT, e.g. \"0.0.0.0:2323\" (empty = off)");
    s.integer("console.telnet_max_sessions", 4, "Telnet sessions at once").range(1, 64);
    s.integer("console.telnet_idle_s", 0, "Close idle telnet sessions after (0 = never)").range(0, 86400).unit("s");

    s.section("forge", "The links of every forgecpp program: commands, events and streams (ZeroMQ)");
    s.string("forge.control", "", "Commands (c2 frames: forgectl, other programs), e.g. \"tcp://*:5557\"");
    s.string("forge.events", "", "PUB: state, alarms, log, audit, health, metrics, e.g. \"tcp://*:5559\"");
    s.string("forge.data", "", "PUB: the application's streams (empty: an ephemeral port when used)");
    s.integer("forge.metrics_interval_s", 5, "Metrics event interval").range(1, 3600).unit("s");
    s.integer("forge.control_workers", 2, "Commands on [forge].control handled at the same time").range(1, 16);

    s.section("serial", "The same commands on a serial line (c2 frames, zephyrus framing)");
    s.string("serial.device", "", "The tty, e.g. \"/dev/ttyUSB0\" (empty = off); opened again when it goes away");
    s.integer("serial.baud", 115200, "Baud rate").range(9600, 4000000);
    s.integer("serial.frame_size", 244, "Largest frame sent; the peer's CONFIG_ZEPHYRUS_COBS_FRAME_SIZE")
        .range(16, 4096)
        .unit("B");
    s.section("can", "The same commands on a CAN bus (c2 frames, zephyrus framing, over ISO-TP)");
    s.string("can.interface", "", "The CAN interface, e.g. \"can0\" (empty = off); opened again when it goes away");
    s.integer("can.tx_id", 0x701, "ISO-TP id this program answers with").range(0, 0x1FFFFFFF);
    s.integer("can.rx_id", 0x700, "ISO-TP id this program listens to (the peer's tx id)").range(0, 0x1FFFFFFF);
    s.boolean("can.extended", false, "29-bit ids");
    s.boolean("can.fd", false, "CAN FD frames (64 bytes) under ISO-TP");
    s.integer("can.frame_size", 244,
              "Largest framed piece in one ISO-TP message; the peer's CONFIG_ZEPHYRUS_COBS_FRAME_SIZE")
        .range(16, 4095)
        .unit("B");
    s.alias("forge.serial", "serial.device");
    s.alias("forge.serial_baud", "serial.baud");
    s.alias("forge.serial_frame_size", "serial.frame_size");

    s.section("loop", "The main loop");
    s.integer("loop.slow_task_ms", 200,
              "Warn when one main loop task (timer, control, command) runs longer than this (0 = never)")
        .range(0, 60000)
        .unit("ms");
    s.integer("loop.request_timeout_ms", 2000,
              "How long a request (a command, HTTP) waits for a busy main loop before it is refused")
        .range(10, 600000)
        .unit("ms");
    s.integer("loop.max_pending_tasks", 10000,
              "Tasks the main loop may have waiting; beyond that post() drops them (forge.loop.dropped)")
        .range(16, 10000000);
    s.alias("forge.slow_task_ms", "loop.slow_task_ms");
    s.alias("forge.request_timeout_ms", "loop.request_timeout_ms");
    s.alias("forge.max_pending_tasks", "loop.max_pending_tasks");

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
    s.integer("health.stall_ms", 5000, "/health says \"stalled\" when the main loop's heartbeat is older than this")
        .range(1000, 600000)
        .unit("ms");
    s.number("health.watchdog_grace_s", 0,
             "Stop systemd watchdog pings after a critical component failed this long (0 = never)")
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

bool Application::Impl::validate_config(const Config& cfg, bool at_start) {
    const auto r = schema.validate(cfg);
    for (const auto& w : r.warnings) {
        if (at_start && !logger) {
            std::cerr << info.name << ": warning: " << w << "\n";
        } else if (logger) {
            logger->warn("config: {}", w);
        }
    }
    for (const auto& e : r.errors) {
        if (logger) {
            logger->error("config: {}", e);
        } else {
            std::cerr << info.name << ": config: " << e << "\n";
        }
    }
    return r.ok();
}

}  // namespace forge
