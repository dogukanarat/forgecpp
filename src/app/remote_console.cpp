// RemoteConsole: one command registry for the local REPL (-i) and telnet,
// with the framework's commands.
#include "impl.hpp"

#include "forge/component.hpp"
#include "forge/control.hpp"
#include "forge/trace.hpp"
#include "forge/version.hpp"
#include "forge/web.hpp"

#include <unistd.h>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <future>

namespace forge::detail {

namespace {
std::string format_duration(std::chrono::steady_clock::duration d) {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(d).count();
    const auto days = secs / 86400;
    secs %= 86400;
    return days > 0 ? std::format("{}d {:02}:{:02}:{:02}", days, secs / 3600, (secs % 3600) / 60, secs % 60)
                    : std::format("{:02}:{:02}:{:02}", secs / 3600, (secs % 3600) / 60, secs % 60);
}
}  // namespace

void RemoteConsole::create(const Config& cfg) {
    const auto& info = app_.info();
    // The command registry always exists (telnet uses it); the local REPL
    // only runs with [console].enabled / -i.
    Console::Options o;
    o.prompt = cfg.value<std::string>("console.prompt");
    o.history_file = cfg.value<std::string>("console.history");
    if (o.history_file.empty()) {
        if (const char* home = std::getenv("HOME")) o.history_file = std::string(home) + "/." + info.name + "_history";
    }
    o.color = cfg.get_or<bool>("console.color", ::isatty(STDOUT_FILENO) != 0);  // computed default
    o.banner = std::format("{} {} - type 'help' for commands, 'exit' to quit", info.name, info.version);

    console_ = std::make_unique<Console>(o);
    // Commands run on the main loop so they never race with application state.
    console_->set_executor([this](std::function<void()> fn) {
        if (app_.stopping()) return;
        if (app_.on_main_loop()) {
            fn();
            return;
        }
        // Not invoke(): the wait gives up when the application stops, so the
        // REPL thread can be joined while the loop is shutting down.
        auto task = std::make_shared<std::packaged_task<void()>>(std::move(fn));
        auto future = task->get_future();
        if (!d_.loop.post([task] { (*task)(); })) d_.loop.busy();
        while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
            if (app_.stopping()) return;
        }
        future.get();
    });
    console_->on_exit([this] { app_.stop(0); });

    register_builtins(*console_);
    if (app_.dashboard()) register_controls(*console_);
    register_framework(*console_);
    d_.setup_console(*console_);  // the application's commands, its components', the hooks'
}

void RemoteConsole::start_telnet(const Config& cfg) {
    const auto listen = cfg.value<std::string>("console.telnet");
    if (listen.empty() || !console_) return;
    const auto& info = app_.info();
    Telnet::Options o;
    o.listen = listen;
    o.max_sessions = cfg.value<std::size_t>("console.telnet_max_sessions");
    o.idle_s = cfg.value<unsigned>("console.telnet_idle_s");
    o.prompt = cfg.value<std::string>("console.prompt");
    o.banner = std::format("{} {} on {} (forgecpp {})", info.name, info.version, host_name(), version_string);
    telnet_ = std::make_unique<Telnet>(*console_, app_.auth(), o);
    telnet_->start();
    d_.endpoints.set("telnet", telnet_->endpoint());
    app_.logger()->info("console: telnet on {}", telnet_->endpoint());
}

void RemoteConsole::stop() {
    if (telnet_) telnet_->stop();
    if (console_) console_->stop();
}

void RemoteConsole::reset() {
    telnet_.reset();
    console_.reset();
}

void RemoteConsole::register_builtins(Console& c) {
    c.add("status", "status  show application status", [this, &c](const Console::Args&) {
        const auto& info = app_.info();
        c.print("{} {} (forgecpp {})", info.name, info.version, version_string);
        c.print("  pid          {}", ::getpid());
        c.print("  uptime       {}", format_duration(app_.uptime()));
        c.print("  log level    {}", log::to_string(log::level()));
        c.print("  workers      {} ({} tasks queued)", app_.pool().get_thread_count(), app_.pool().get_tasks_queued());
        c.print("  timers       {}", app_.scheduler().pending());
        if (auto* http = app_.http()) c.print("  http         port {}", http->port());
    });
    c.add("log-level", "log-level [level]  show or change the log level", [this, &c](const Console::Args& args) {
            // log-level <level> | log-level <logger> <level>
        if (args.size() == 1) app_.control().set_log_level({}, args[0], console_caller());
        if (args.size() >= 2) app_.control().set_log_level(args[0], args[1], console_caller());
        c.print("log level: {}", log::to_string(log::level()));
        for (const auto& [name, lvl] : log::logger_levels()) c.print("  {:<24} {}", name, lvl);
    }, [](const Console::Args& so_far) -> std::vector<std::string> {
        if (!so_far.empty()) return {};
        return {"trace", "debug", "info", "warn", "error", "critical", "off"};
    });
    c.add("config", "config [key.path]  print the effective configuration as JSON",
          [this, &c](const Console::Args& args) {
        const auto cfg = app_.config();
        if (args.empty()) {
            c.print("{}", cfg.to_json().dump(2));
        } else if (auto v = cfg.get<json>(args[0])) {
            c.print("{}", v->dump(2));
        } else {
            c.error("no such key '" + args[0] + "'");
        }
    });
    c.add("reload", "reload  re-read the configuration file", [this](const Console::Args&) { d_.reload(); });
    c.add("alarms", "alarms  list active alarms and recent history", [this, &c](const Console::Args&) {
        const auto list = app_.alarms().list();
        if (list.empty()) {
            c.print("no alarms");
            return;
        }
        for (const auto& a : list) {
            const auto t = Alarm::Clock::to_time_t(a.active ? a.last_at : *a.cleared_at);
            std::tm tm{};
            ::localtime_r(&t, &tm);
            c.print("  {:<8} {:<9} {:<20} {:02}:{:02}:{:02} {}{}", to_string(a.severity),
                    a.pending() ? "ACTIVE" : (a.active ? "ACKED" : "CLEARED"), a.id, tm.tm_hour, tm.tm_min, tm.tm_sec,
                    a.message, a.count > 1 ? std::format(" (x{})", a.count) : "");
        }
    });
    c.add("ack", "ack <id|all>  acknowledge an alarm", [this, &c](const Console::Args& args) {
        if (args.empty()) throw std::invalid_argument("usage: ack <id|all>");
        c.print("{} alarm(s) acknowledged", app_.control().ack(args[0], console_caller()));
    }, [this](const Console::Args& so_far) -> std::vector<std::string> {
        if (!so_far.empty()) return {};
        std::vector<std::string> ids{"all"};
        for (const auto& a : app_.alarms().list()) ids.push_back(a.id);
        return ids;
    });
}

void RemoteConsole::register_controls(Console& c) {
    auto* dashboard = app_.dashboard();
    auto& control = app_.control();
    auto ids = [dashboard](const Console::Args& so_far) -> std::vector<std::string> {
        if (!so_far.empty()) return {};
        return dashboard->ids();
    };
    c.add("controls", "controls  list the web dashboard controls and their values",
          [dashboard, &c](const Console::Args&) {
        const auto schema = dashboard->schema();
        for (const auto& g : schema["groups"]) {
            c.print("[{}]", g["title"].get<std::string>());
            for (const auto& ctl : g["controls"]) {
                const auto value = ctl.contains("value") ? ctl["value"].dump() : std::string("(button)");
                c.print("  {:<20} {:<7} {}{}", ctl["id"].get<std::string>(), ctl["kind"].get<std::string>(), value,
                        ctl.contains("unit") ? " " + ctl["unit"].get<std::string>() : "");
            }
        }
    });
    c.add("set", "set <id> <value>  change a dashboard control (value as JSON: 42, true, \"text\")",
          [&control, &c](const Console::Args& args) {
        if (args.size() < 2) throw std::invalid_argument("usage: set <id> <value>");
        std::string text;
        for (std::size_t i = 1; i < args.size(); ++i) text += (i > 1 ? " " : "") + args[i];
        auto value = json::parse(text, nullptr, false);
        if (value.is_discarded()) value = text;  // bare words are strings
        const auto now = control.set(args[0], value, console_caller());
        c.print("{} = {}", args[0], now.dump());
    }, ids);
    c.add("press", "press <id>  press a dashboard button", [&control](const Console::Args& args) {
        if (args.empty()) throw std::invalid_argument("usage: press <id>");
        control.press(args[0], console_caller());
    }, ids);
}

void RemoteConsole::register_framework(Console& c) {
    auto& control = app_.control();
    c.add("metrics", "metrics [prefix]  show metrics", [&c](const Console::Args& args) {
        const auto snap = metrics::registry().snapshot(args.empty() ? "" : args[0]);
        for (const auto& m : snap.metrics) {
            const std::string unit = m.unit.empty() ? "" : " " + m.unit;
            if (m.type == metrics::Type::histogram) {
                const double mean = m.count ? m.sum / static_cast<double>(m.count) : 0;
                c.print("  {:<36} count {} mean {:.6g}{}", m.name, m.count, mean, unit);
            } else {
                c.print("  {:<36} {:.6g}{}", m.name, m.value, unit);
            }
        }
    });
    c.add("trace", "trace [seconds] [file]  save the flight recorder (Chrome JSON, open in ui.perfetto.dev)",
          [&c](const Console::Args& args) {
        const unsigned seconds = args.empty() ? 0 : static_cast<unsigned>(std::stoul(args[0]));
        const std::string file = args.size() > 1 ? args[1] : "/tmp/forge-trace.json";
        auto d = trace::dump(seconds);
        std::ofstream(file) << d.chrome_json;
        c.print("{} events written to {} ({} overwritten)", d.events, file, d.dropped);
    });
    c.add("audit", "audit [n]  recent control changes", [this, &c](const Console::Args& args) {
        const std::size_t n = args.empty() ? 20 : std::stoul(args[0]);
        for (const auto& r : app_.audit().list(n)) {
            const auto t = static_cast<std::time_t>(r.time_unix_ns / 1000000000);
            std::tm tm{};
            ::localtime_r(&t, &tm);
            c.print("  {:02}:{:02}:{:02} {:<26} {:<7} {:<20} {}{}", tm.tm_hour, tm.tm_min, tm.tm_sec,
                    (r.user.empty() ? r.channel : r.user + "@" + r.channel) + " " + r.client, r.action, r.target,
                    r.new_value.is_null() ? "" : r.new_value.dump(), r.ok ? "" : " REFUSED: " + r.error);
        }
    });
    c.add("health", "health  component health", [this, &c](const Console::Args&) {
        const auto h = app_.health().snapshot();
        c.print("overall: {}", to_string(h.state));
        for (const auto& comp : h.components) {
            c.print("  {:<20} {:<9} {}{}", comp.name, to_string(comp.state), comp.message,
                    comp.critical ? "" : " (non-critical)");
        }
    });
    c.add("streams", "streams  published streams and endpoints", [this, &c](const Console::Args&) {
        for (const auto& [k, v] : app_.endpoints()) c.print("  {:<8} {}", k, v);
        for (const auto& s : d_.messaging.stream_list()) {
            c.print("  stream {:<16} {:<22} {} subscriber(s), {} sent, {} dropped", s.name, s.type, s.subscribers,
                    s.sent, s.dropped);
        }
    });
    c.add("call", "call <command> [name=value ...]  run a command", [&control, &c](const Console::Args& args) {
        if (args.empty()) throw std::invalid_argument("usage: call <command> [name=value ...]");
        json a = json::object();
        for (std::size_t i = 1; i < args.size(); ++i) {
            const auto eq = args[i].find('=');
            if (eq == std::string::npos) throw std::invalid_argument("arguments are name=value");
            auto v = json::parse(args[i].substr(eq + 1), nullptr, false);
            a[args[i].substr(0, eq)] = v.is_discarded() ? json(args[i].substr(eq + 1)) : v;
        }
        const auto result = control.call(args[0], a, console_caller());
        if (!result.is_null()) c.print("{}", result.dump(2));
    }, [this](const Console::Args& so_far) -> std::vector<std::string> {
        if (!so_far.empty()) return {};
        auto* d = app_.dashboard();
        return d ? d->command_ids() : std::vector<std::string>{};
    });
    c.add("lease", "lease <id|group:title> [seconds]  hold a control so nobody else changes it",
          [&control, &c](const Console::Args& args) {
        if (args.empty()) throw std::invalid_argument("usage: lease <id> [seconds]");
        const auto s = control.lease(args[0], args.size() > 1 ? std::stoul(args[1]) : 60, console_caller());
        c.print("{} leased for {} s", args[0], s.count());
    });
    c.add("release", "release <id>  give a lease back", [&control](const Console::Args& args) {
        if (args.empty()) throw std::invalid_argument("usage: release <id>");
        control.release(args[0], console_caller());
    });
}

}  // namespace forge::detail
