// forgectl: one command line client for every forgecpp application (forge.v1).
//
//   forgectl -e tcp://board:5557 info
//   forgectl -e tcp://board:5557 controls
//   forgectl -e tcp://board:5557 set rx_lo_mhz 2437
//   forgectl -e tcp://board:5557 call capture seconds=2
//   forgectl -e tcp://board:5557 watch            # events: state, alarms, log, audit, health
//   forgectl -e tcp://board:5557 trace -s 5 -o trace.json
//   forgectl -e http://board:8080 state           # same requests over HTTP
//
// Endpoint: tcp://host:port ([forge].control) or http://host:port (/forge/api/rpc).
// FORGE_ENDPOINT and FORGE_TOKEN may be set instead of -e / --token.

#include <forge/log.hpp>
#include <forge/proto.hpp>
#include <forge/zmq.hpp>

#include <CLI/CLI.hpp>
#include <httplib.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <thread>

#include <pwd.h>
#include <unistd.h>

using forge::json;
namespace v1 = forge::v1;
namespace proto = forge::proto;

namespace {

std::atomic<bool> g_stop{false};

struct Target {
    std::string endpoint, token;
    int timeout_ms = 3000;
    bool as_json = false;
};

class Transport {
public:
    explicit Transport(const Target& t) : t_(t) {
        if (t.endpoint.rfind("http://", 0) == 0 || t.endpoint.rfind("https://", 0) == 0) {
            http_ = std::make_unique<httplib::Client>(t.endpoint);
            http_->set_read_timeout(std::chrono::milliseconds(t.timeout_ms));
            http_->set_connection_timeout(std::chrono::milliseconds(t.timeout_ms));
        } else {
            zmq_ = std::make_unique<forge::zmq::Client>(t.endpoint, std::chrono::milliseconds(t.timeout_ms));
            zmq_->set_token(t.token);
        }
    }

    v1::Reply call(v1::Request r) {
        r.set_client(client_name());
        if (!t_.token.empty()) r.set_token(t_.token);
        v1::Reply reply;
        if (zmq_) {
            reply = zmq_->request(std::move(r));
        } else {
            std::string body;
            r.SerializeToString(&body);
            httplib::Headers h;
            if (!t_.token.empty()) h.emplace("Authorization", "Bearer " + t_.token);
            auto res = http_->Post("/forge/api/rpc", h, body, "application/x-protobuf");
            if (!res) throw std::runtime_error("no reply from " + t_.endpoint);
            if (!reply.ParseFromString(res->body)) {
                throw std::runtime_error("HTTP " + std::to_string(res->status) + ": " + res->body);
            }
        }
        if (!reply.ok()) throw std::runtime_error(std::to_string(reply.status()) + ": " + reply.error());
        return reply;
    }

private:
    static std::string client_name() {
        char host[128] = {};
        ::gethostname(host, sizeof(host) - 1);
        const char* user = std::getenv("USER");
        std::string name = user && *user ? user : "";
        if (name.empty()) {
            if (const passwd* pw = ::getpwuid(::getuid())) name = pw->pw_name;  // services, containers
        }
        return "forgectl:" + (name.empty() ? std::to_string(::getuid()) : name) + "@" + host;
    }
    const Target& t_;
    std::unique_ptr<forge::zmq::Client> zmq_;
    std::unique_ptr<httplib::Client> http_;
};

/// A log level for the terminal: in its colour when stdout is one (and NO_COLOR is not set).
std::string level_text(const std::string& level) {
    static const bool color = ::isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR");
    if (!color) return level;
    try {
        return std::string(forge::log::level_color(forge::log::parse_level(level))) + level + "\033[0m";
    } catch (const std::exception&) {
        return level;
    }
}

std::string clock_time(std::int64_t unix_ns) {
    const auto t = static_cast<std::time_t>(unix_ns / 1000000000);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

json parse_value(const std::string& text) {
    auto v = json::parse(text, nullptr, false);
    return v.is_discarded() ? json(text) : v;
}

void print(const google::protobuf::Message& m) { std::cout << proto::to_json_string(m, true) << "\n"; }

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"forgectl - control any forgecpp application (forge.v1)"};
    Target t;
    if (const char* e = std::getenv("FORGE_ENDPOINT")) t.endpoint = e;
    if (const char* k = std::getenv("FORGE_TOKEN")) t.token = k;
    app.add_option("-e,--endpoint", t.endpoint, "tcp://host:port ([forge].control) or http://host:port");
    app.add_option("--token", t.token, "Credentials ([auth.users.<name>].token)");
    app.add_option("--timeout", t.timeout_ms, "Reply timeout in ms");
    app.add_flag("--json", t.as_json, "Print replies as JSON");
    app.require_subcommand(1);

    std::string id, value, logger_name, level, out_file, prefix, events_endpoint;
    std::vector<std::string> args, topics;
    unsigned limit = 50, seconds = 0, lease_seconds = 60;

    auto* info = app.add_subcommand("info", "Application, version, endpoints");
    auto* controls = app.add_subcommand("controls", "Controls and commands with their values");
    auto* state = app.add_subcommand("state", "Current value of every control");
    auto* get = app.add_subcommand("get", "Value of one control");
    get->add_option("id", id)->required();
    auto* set = app.add_subcommand("set", "Change a control (value as JSON: 42, true, \"text\")");
    set->add_option("id", id)->required();
    set->add_option("value", value)->required();
    auto* press = app.add_subcommand("press", "Press a button");
    press->add_option("id", id)->required();
    auto* call = app.add_subcommand("call", "Run a command: call <id> name=value ...");
    call->add_option("id", id)->required();
    call->add_option("args", args);
    auto* alarms = app.add_subcommand("alarms", "Active and recent alarms");
    auto* ack = app.add_subcommand("ack", "Acknowledge an alarm (or all)");
    ack->add_option("id", id)->required();
    auto* logs = app.add_subcommand("logs", "Recent log records");
    logs->add_option("-n,--limit", limit);
    auto* metrics = app.add_subcommand("metrics", "Metrics");
    metrics->add_option("prefix", prefix);
    auto* trace = app.add_subcommand("trace", "Save the flight recorder (open in ui.perfetto.dev)");
    trace->add_option("-s,--seconds", seconds, "Last N seconds (0 = everything recorded)");
    trace->add_option("-o,--output", out_file, "File (default trace-<app>.json)");
    auto* audit = app.add_subcommand("audit", "Who changed what");
    audit->add_option("-n,--limit", limit);
    auto* health = app.add_subcommand("health", "Component health");
    auto* streams = app.add_subcommand("streams", "Published streams");
    auto* lease = app.add_subcommand("lease", "Hold a control (or group:<title>) so others cannot change it");
    lease->add_option("id", id)->required();
    lease->add_option("seconds", lease_seconds);
    auto* release = app.add_subcommand("release", "Give a lease back");
    release->add_option("id", id)->required();
    auto* log_level = app.add_subcommand("log-level", "Change a log level: log-level <level> | <logger> <level>");
    log_level->add_option("a", logger_name)->required();
    log_level->add_option("b", level);
    auto* watch = app.add_subcommand("watch", "Follow the events: state, alarms, log, audit, health, metrics");
    watch->add_option("topics", topics, "Topics (default: all but metrics)");
    watch->add_option("--events", events_endpoint, "Events endpoint (default: from info)");

    CLI11_PARSE(app, argc, argv);
    if (t.endpoint.empty()) {
        std::cerr << "forgectl: give the application's endpoint with -e (or FORGE_ENDPOINT)\n";
        return 2;
    }
    std::signal(SIGINT, [](int) { g_stop = true; });

    try {
        Transport tr(t);
        v1::Request r;
        auto show = [&](const google::protobuf::Message& m) {
            if (t.as_json) print(m);
        };

        if (info->parsed()) {
            r.mutable_info();
            const auto i = tr.call(r).info();
            if (t.as_json) return print(i), 0;
            std::cout << i.name() << " " << i.version() << " on " << i.host() << " (pid " << i.pid() << ", up "
                      << i.uptime_s() << " s, forgecpp " << i.forgecpp() << ", " << i.protocol() << ")\n";
            if (!i.description().empty()) std::cout << i.description() << "\n";
            for (const auto& [k, v] : i.endpoints()) std::cout << "  " << k << ": " << v << "\n";
        } else if (controls->parsed()) {
            r.mutable_schema();
            const auto s = tr.call(r).schema();
            if (t.as_json) return print(s), 0;
            std::string group;
            for (const auto& c : s.controls()) {
                if (c.group() != group) std::cout << "[" << (group = c.group()) << "]\n";
                std::cout << "  " << c.id() << "  " << c.kind();
                if (c.kind() != "button") std::cout << "  " << proto::to_json(c.value()).dump();
                if (!c.unit().empty()) std::cout << " " << c.unit();
                if (!c.lease_holder().empty()) std::cout << "  (leased by " << c.lease_holder() << ")";
                std::cout << "\n";
            }
            for (const auto& c : s.commands()) {
                std::cout << "  " << c.id() << "  command(";
                for (int i = 0; i < c.params_size(); ++i) std::cout << (i ? ", " : "") << c.params(i).name();
                std::cout << ")\n";
            }
        } else if (state->parsed()) {
            r.mutable_state();
            const auto v = tr.call(r).state();
            if (t.as_json) return print(v), 0;
            for (const auto& [k, val] : v.values()) std::cout << k << " = " << proto::to_json(val).dump() << "\n";
        } else if (get->parsed()) {
            r.mutable_get()->set_id(id);
            std::cout << proto::to_json(tr.call(r).value()).dump() << "\n";
        } else if (set->parsed()) {
            r.mutable_set()->set_id(id);
            *r.mutable_set()->mutable_value() = proto::to_value(parse_value(value));
            std::cout << id << " = " << proto::to_json(tr.call(r).value()).dump() << "\n";
        } else if (press->parsed()) {
            r.mutable_press()->set_id(id);
            tr.call(r);
        } else if (call->parsed()) {
            json a = json::object();
            for (const auto& kv : args) {
                const auto eq = kv.find('=');
                if (eq == std::string::npos) throw std::runtime_error("arguments are name=value");
                a[kv.substr(0, eq)] = parse_value(kv.substr(eq + 1));
            }
            r.mutable_call()->set_id(id);
            *r.mutable_call()->mutable_args() = proto::to_struct(a);
            const auto v = proto::to_json(tr.call(r).value());
            if (!v.is_null()) std::cout << v.dump(2) << "\n";
        } else if (alarms->parsed()) {
            r.mutable_alarms();
            const auto a = tr.call(r).alarms();
            if (t.as_json) return print(a), 0;
            if (a.alarms().empty()) std::cout << "no alarms\n";
            for (const auto& x : a.alarms()) {
                std::cout << proto::severity_name(x.severity()) << "  "
                          << (x.active() ? (x.acknowledged() ? "ACKED  " : "ACTIVE ") : "CLEARED") << "  " << x.id()
                          << "  " << x.message() << "\n";
            }
        } else if (ack->parsed()) {
            r.mutable_ack()->set_id(id);
            std::cout << tr.call(r).count() << " alarm(s) acknowledged\n";
        } else if (logs->parsed()) {
            r.mutable_logs()->set_limit(limit);
            const auto l = tr.call(r).logs();
            if (t.as_json) return print(l), 0;
            for (const auto& x : l.records()) {
                std::cout << clock_time(x.time_unix_ns()) << " [" << level_text(x.level()) << "] [" << x.logger() << "] "
                          << x.message() << "\n";
            }
        } else if (metrics->parsed()) {
            r.mutable_metrics()->set_prefix(prefix);
            const auto m = tr.call(r).metrics();
            if (t.as_json) return print(m), 0;
            for (const auto& x : m.metrics()) {
                if (x.type() == v1::Metric::TYPE_HISTOGRAM) {
                    std::cout << x.name() << "  count " << x.count() << "  mean "
                              << (x.count() ? x.sum() / static_cast<double>(x.count()) : 0) << " " << x.unit() << "\n";
                } else {
                    std::cout << x.name() << "  " << x.value() << " " << x.unit() << "\n";
                }
            }
        } else if (trace->parsed()) {
            r.mutable_trace_dump()->set_seconds(seconds);
            const auto d = tr.call(r).trace_dump();
            if (out_file.empty()) out_file = "trace.json";
            std::ofstream(out_file) << d.chrome_json();
            std::cout << d.events() << " events saved to " << out_file << " (open in https://ui.perfetto.dev)\n";
        } else if (audit->parsed()) {
            r.mutable_audit()->set_limit(limit);
            const auto a = tr.call(r).audit();
            if (t.as_json) return print(a), 0;
            for (const auto& x : a.records()) {
                std::cout << clock_time(x.time_unix_ns()) << "  " << (x.user().empty() ? "" : x.user() + "@")
                          << x.channel() << " " << x.client() << "  " << x.action() << " " << x.target();
                if (x.has_new_value()) std::cout << " = " << proto::to_json(x.new_value()).dump();
                if (!x.ok()) std::cout << "  REFUSED: " << x.error();
                std::cout << "\n";
            }
        } else if (health->parsed()) {
            r.mutable_health();
            print(tr.call(r).health());
        } else if (streams->parsed()) {
            r.mutable_streams();
            print(tr.call(r).streams());
        } else if (lease->parsed()) {
            r.mutable_lease()->set_id(id);
            r.mutable_lease()->set_seconds(lease_seconds);
            std::cout << id << " leased for " << tr.call(r).count() << " s\n";
        } else if (release->parsed()) {
            r.mutable_release()->set_id(id);
            tr.call(r);
        } else if (log_level->parsed()) {
            if (level.empty()) {
                r.mutable_log_level()->set_level(logger_name);
            } else {
                r.mutable_log_level()->set_logger(logger_name);
                r.mutable_log_level()->set_level(level);
            }
            tr.call(r);
        } else if (watch->parsed()) {
            if (events_endpoint.empty()) {
                r.mutable_info();
                const auto i = tr.call(r).info();
                auto it = i.endpoints().find("events");
                if (it == i.endpoints().end()) throw std::runtime_error("the application has no [forge].events endpoint");
                events_endpoint = it->second;
                // "tcp://0.0.0.0:5559" -> the host we reached
                const auto star = events_endpoint.find("0.0.0.0");
                if (star != std::string::npos && t.endpoint.rfind("tcp://", 0) == 0) {
                    const auto host = t.endpoint.substr(6, t.endpoint.rfind(':') - 6);
                    events_endpoint.replace(star, 7, host);
                }
            }
            if (topics.empty()) topics = {"state", "alarms", "log", "audit", "health"};
            forge::zmq::Receiver::Options o;
            o.topics = topics;
            std::cerr << "watching " << events_endpoint << " (Ctrl-C to stop)\n";
            forge::zmq::Receiver sub(
                forge::zmq::Receiver::Kind::sub, events_endpoint,
                [&](forge::zmq::Message&& m) {
                    const auto when = clock_time(m.header.time_unix_ns());
                    if (m.topic == "log") {
                        const auto x = m.as<v1::LogRecord>();
                        std::cout << when << " log   [" << level_text(x.level()) << "] [" << x.logger() << "] " << x.message() << "\n";
                    } else if (m.topic == "state") {
                        const auto values = m.as<v1::Values>();
                        for (const auto& [k, v] : values.values()) {
                            std::cout << when << " state " << k << " = " << proto::to_json(v).dump() << "\n";
                        }
                    } else if (m.topic == "audit") {
                        const auto x = m.as<v1::AuditRecord>();
                        std::cout << when << " audit " << (x.user().empty() ? "" : x.user() + "@") << x.channel() << " "
                                  << x.action() << " " << x.target()
                                  << (x.has_new_value() ? " = " + proto::to_json(x.new_value()).dump() : "")
                                  << (x.ok() ? "" : " REFUSED: " + x.error()) << "\n";
                    } else if (m.topic == "alarms") {
                        const auto list = m.as<v1::AlarmList>();
                        for (const auto& a : list.alarms()) {
                            if (a.active() && !a.acknowledged()) {
                                std::cout << when << " alarm " << proto::severity_name(a.severity()) << " " << a.id()
                                          << ": " << a.message() << "\n";
                            }
                        }
                    } else if (m.topic == "health") {
                        std::cout << when << " health " << proto::to_json_string(m.as<v1::Health>()) << "\n";
                    } else if (m.topic == "metrics") {
                        std::cout << when << " metrics " << m.as<v1::MetricsSnapshot>().metrics_size() << " values\n";
                    }
                    std::cout.flush();
                },
                o);
            while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        (void)show;
    } catch (const std::exception& e) {
        std::cerr << "forgectl: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
