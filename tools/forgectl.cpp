// forgectl: one command line client for every forgecpp application.
//
//   forgectl -e tcp://board:5557 info
//   forgectl -e tcp://board:5557 controls
//   forgectl -e tcp://board:5557 set rx_lo_mhz 2437
//   forgectl -e tcp://board:5557 call capture seconds=2
//   forgectl -e tcp://board:5557 watch            # events: state, alarms, log, audit, health
//   forgectl -e tcp://board:5557 trace -s 5 -o trace.json
//   forgectl -e tcp://board:5557 tmtc                    # every command, with its number
//   forgectl -e tcp://board:5557 tmtc history '{"count": 3}'   # any command, in JSON
//   forgectl -e tcp://board:5557 stream spectrum -n 5    # messages of a stream, in JSON
//   forgectl -e http://board:8080 state           # the same commands over HTTP (JSON)
//
// Endpoint: tcp://host:port ([forge].control, c2 frames) or http://host:port
// (/forge/api/tmtc). FORGE_ENDPOINT and FORGE_TOKEN may be set instead of -e / --token.

#include <forge/log.hpp>
#include <forge/messages.forge.hpp>
#include <forge/pb.hpp>
#include <forge/zmq.hpp>

#include <httplib.h>
#include <CLI/CLI.hpp>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <thread>

#include <unistd.h>

using forge::json;
namespace pb = forge::pb;

namespace {

std::atomic<bool> g_stop{false};

struct Target {
    std::string endpoint, token;
    int timeout_ms = 3000;
    bool as_json = false;
};

// The same commands over ZeroMQ (binary) or HTTP (JSON).
class Link {
public:
    explicit Link(const Target& t) : t_(t) {
        if (is_http()) {
            http_ = std::make_unique<httplib::Client>(t.endpoint);
            http_->set_read_timeout(std::chrono::milliseconds(t.timeout_ms));
            http_->set_connection_timeout(std::chrono::milliseconds(t.timeout_ms));
        } else {
            zmq_ = std::make_unique<forge::zmq::Client>(t.endpoint, std::chrono::milliseconds(t.timeout_ms));
        }
    }

    bool is_http() const { return t_.endpoint.rfind("http://", 0) == 0 || t_.endpoint.rfind("https://", 0) == 0; }

    /// Every reply of a command (several for a list).
    template <pb::Message Req, pb::Message Rsp>
    void each(std::uint16_t id, const std::string& name, const Req& req, const std::function<void(const Rsp&)>& fn) {
        if (zmq_) {
            if (auto s = zmq_->call_parts<Req, Rsp>(id, req, fn); !s.ok()) throw std::runtime_error(s.to_string());
            return;
        }
        httplib::Headers h;
        if (!t_.token.empty()) h.emplace("Authorization", "Bearer " + t_.token);
        auto res = http_->Post("/forge/api/tmtc/" + name, h, pb::to_json(req).dump(), "application/json");
        if (!res) throw std::runtime_error("no answer from " + t_.endpoint);
        const auto body = json::parse(res->body, nullptr, false);
        if (res->status != 200) {
            throw std::runtime_error("HTTP " + std::to_string(res->status) + ": " +
                                     (body.is_object() ? body.value("error", res->body) : res->body));
        }
        if (body.is_array()) {
            for (const auto& part : body) fn(pb::from_json<Rsp>(part));
        } else {
            fn(pb::from_json<Rsp>(body));
        }
    }

    template <pb::Message Req, pb::Message Rsp>
    Rsp one(std::uint16_t id, const std::string& name, const Req& req) {
        Rsp out{};
        each<Req, Rsp>(id, name, req, [&](const Rsp& r) { out = r; });
        return out;
    }

private:
    const Target& t_;
    std::unique_ptr<forge::zmq::Client> zmq_;
    std::unique_ptr<httplib::Client> http_;
};

template <std::size_t N>
void set_text(char (&dst)[N], const std::string& src) {
    if (!pb::assign(dst, src).ok()) throw std::runtime_error("'" + src + "' is too long");
}

/// A log level for the terminal: three letters (TRC .. CRT), in its colour when stdout
/// is a terminal (and NO_COLOR is not set).
std::string level_text(std::string_view level) {
    static const bool color = ::isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR");
    try {
        const auto l = forge::log::parse_level(level);
        const std::string name(forge::log::short_name(l));
        return color ? std::string(forge::log::level_color(l)) + name + "\033[0m" : name;
    } catch (const std::exception&) {
        return std::string(level);
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

// A value as the user typed it: JSON if it is JSON, else text.
json parse_value(const std::string& text) {
    auto v = json::parse(text, nullptr, false);
    return v.is_discarded() ? json(text) : v;
}

const char* severity_name(forge_severity s) {
    switch (s) {
        case forge_severity_info_level: return "info";
        case forge_severity_warning: return "warning";
        case forge_severity_error: return "error";
        case forge_severity_critical: return "critical";
        default: return "?";
    }
}

template <pb::Message T>
void print_json(const T& m) {
    std::cout << pb::to_json(m).dump(2) << "\n";
}

void print_alarm(const std::string& prefix, const forge_alarm& a) {
    std::cout << prefix << severity_name(a.severity) << "  "
              << (a.active ? (a.acknowledged ? "ACKED  " : "ACTIVE ") : "CLEARED") << "  " << a.id << "  " << a.message
              << "\n";
}

// "tcp://0.0.0.0:5559" from the application -> the host we reached it at.
std::string reachable(std::string endpoint, const std::string& via) {
    const auto any = endpoint.find("0.0.0.0");
    if (any == std::string::npos) return endpoint;
    const auto scheme = via.find("://");
    const auto colon = via.rfind(':');
    if (scheme == std::string::npos || colon <= scheme + 3) return endpoint;
    return endpoint.replace(any, 7, via.substr(scheme + 3, colon - scheme - 3));
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"forgectl - control any forgecpp application"};
    Target t;
    if (const char* e = std::getenv("FORGE_ENDPOINT")) t.endpoint = e;
    if (const char* k = std::getenv("FORGE_TOKEN")) t.token = k;
    app.add_option("-e,--endpoint", t.endpoint, "tcp://host:port ([forge].control) or http://host:port");
    app.add_option("--token", t.token, "Credentials over HTTP ([auth.users.<name>].token)");
    app.add_option("--timeout", t.timeout_ms, "Reply timeout in ms");
    app.add_flag("--json", t.as_json, "Print replies as JSON");
    app.require_subcommand(1);

    std::string id, value, logger_name, level, out_file, prefix, events_endpoint;
    std::vector<std::string> args, topics;
    unsigned limit = 50, seconds = 0, lease_seconds = 60, count = 0;

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
    trace->add_option("-o,--output", out_file, "File (default trace.json)");
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
    auto* tmtc = app.add_subcommand("tmtc", "List the commands, or run one: tmtc <name|number> ['{json}'] (over HTTP)");
    tmtc->add_option("name", id);
    tmtc->add_option("request", value, "The request as JSON (default {})");
    auto* stream = app.add_subcommand("stream", "Print the messages of a stream as they go out (a few per second)");
    stream->add_option("name", id)->required();
    stream->add_option("-n,--count", count, "Stop after this many (0: until Ctrl-C)");

    CLI11_PARSE(app, argc, argv);
    if (t.endpoint.empty()) {
        std::cerr << "forgectl: give the application's endpoint with -e (or FORGE_ENDPOINT)\n";
        return 2;
    }
    std::signal(SIGINT, [](int) { g_stop = true; });

    try {
        Link link(t);
        const auto endpoint_of = [&](const std::string& name) {
            const auto i = link.one<forge_info_request, forge_info_reply>(forge_msg_id_info, "info", {});
            for (pb_size_t k = 0; k < i.endpoints_count; ++k) {
                if (name == i.endpoints[k].name) return reachable(i.endpoints[k].url, t.endpoint);
            }
            throw std::runtime_error("the application has no " + name + " endpoint");
        };

        if (info->parsed()) {
            const auto i = link.one<forge_info_request, forge_info_reply>(forge_msg_id_info, "info", {});
            if (t.as_json) return print_json(i), 0;
            std::cout << i.name << " " << i.version << " on " << i.host << " (pid " << i.pid << ", up " << i.uptime_s
                      << " s, forgecpp " << i.forgecpp << ")\n";
            if (i.description[0]) std::cout << i.description << "\n";
            for (pb_size_t k = 0; k < i.endpoints_count; ++k) {
                std::cout << "  " << i.endpoints[k].name << ": " << i.endpoints[k].url << "\n";
            }
        } else if (controls->parsed() || state->parsed()) {
            std::string group;
            link.each<forge_controls_request, forge_controls_reply>(forge_msg_id_controls, "controls", {},
                                                                    [&](const forge_controls_reply& page) {
                if (t.as_json) return print_json(page);
                for (pb_size_t k = 0; k < page.controls_count; ++k) {
                    const auto& c = page.controls[k];
                    const bool button = pb::view(c.kind) == "button";
                    if (state->parsed()) {
                        if (!button) std::cout << c.id << " = " << c.value << "\n";
                        continue;
                    }
                    if (group != c.group) std::cout << "[" << (group = c.group) << "]\n";
                    std::cout << "  " << c.id << "  " << c.kind;
                    if (!button) std::cout << "  " << c.value;
                    if (c.unit[0]) std::cout << " " << c.unit;
                    if (c.lease_holder[0]) std::cout << "  (leased by " << c.lease_holder << ")";
                    std::cout << "\n";
                }
            });
            if (controls->parsed() && !t.as_json) {
                link.each<forge_commands_request, forge_commands_reply>(forge_msg_id_commands, "commands", {},
                                                                        [&](const forge_commands_reply& page) {
                    for (pb_size_t k = 0; k < page.commands_count; ++k) {
                        const auto& c = page.commands[k];
                        std::cout << "  " << c.id << "  command(";
                        for (pb_size_t p = 0; p < c.params_count; ++p) std::cout << (p ? ", " : "") << c.params[p].name;
                        std::cout << ")\n";
                    }
                });
            }
        } else if (get->parsed()) {
            forge_get_request req{};
            set_text(req.id, id);
            std::cout << link.one<forge_get_request, forge_get_reply>(forge_msg_id_get, "get", req).value << "\n";
        } else if (set->parsed()) {
            forge_set_request req{};
            set_text(req.id, id);
            set_text(req.value, parse_value(value).dump());
            const auto rsp = link.one<forge_set_request, forge_set_reply>(forge_msg_id_set, "set", req);
            std::cout << id << " = " << rsp.value << "\n";
        } else if (press->parsed()) {
            forge_press_request req{};
            set_text(req.id, id);
            link.one<forge_press_request, forge_press_reply>(forge_msg_id_press, "press", req);
        } else if (call->parsed()) {
            json a = json::object();
            for (const auto& kv : args) {
                const auto eq = kv.find('=');
                if (eq == std::string::npos) throw std::runtime_error("arguments are name=value");
                a[kv.substr(0, eq)] = parse_value(kv.substr(eq + 1));
            }
            forge_call_request req{};
            set_text(req.id, id);
            set_text(req.args, a.dump());
            const auto rsp = link.one<forge_call_request, forge_call_reply>(forge_msg_id_call, "call", req);
            const auto result = json::parse(pb::view(rsp.result), nullptr, false);
            if (!result.is_discarded() && !result.is_null()) std::cout << result.dump(2) << "\n";
        } else if (alarms->parsed()) {
            bool any = false;
            link.each<forge_alarms_request, forge_alarms_reply>(forge_msg_id_alarms, "alarms", {},
                                                                [&](const forge_alarms_reply& page) {
                if (t.as_json) return print_json(page);
                for (pb_size_t k = 0; k < page.alarms_count; ++k) print_alarm("", page.alarms[k]);
                any = any || page.alarms_count > 0;
            });
            if (!any && !t.as_json) std::cout << "no alarms\n";
        } else if (ack->parsed()) {
            forge_ack_request req{};
            set_text(req.id, id);
            std::cout << link.one<forge_ack_request, forge_ack_reply>(forge_msg_id_ack, "ack", req).count
                      << " alarm(s) acknowledged\n";
        } else if (logs->parsed()) {
            forge_logs_request req{};
            req.limit = limit;
            link.each<forge_logs_request, forge_logs_reply>(forge_msg_id_logs, "logs", req,
                                                            [&](const forge_logs_reply& page) {
                if (t.as_json) return print_json(page);
                for (pb_size_t k = 0; k < page.records_count; ++k) {
                    const auto& x = page.records[k];
                    std::cout << clock_time(x.time_unix_ns) << " [" << level_text(x.level) << "] [" << x.logger << "] "
                              << x.message << "\n";
                }
            });
        } else if (metrics->parsed()) {
            forge_metrics_request req{};
            set_text(req.prefix, prefix);
            link.each<forge_metrics_request, forge_metrics_reply>(forge_msg_id_metrics, "metrics", req,
                                                                  [&](const forge_metrics_reply& page) {
                if (t.as_json) return print_json(page);
                for (pb_size_t k = 0; k < page.metrics_count; ++k) {
                    const auto& x = page.metrics[k];
                    if (x.type == forge_metric_type_histogram) {
                        std::cout << x.name << "  count " << x.count << "  mean "
                                  << (x.count ? x.sum / static_cast<double>(x.count) : 0) << " " << x.unit << "\n";
                    } else {
                        std::cout << x.name << "  " << x.value << " " << x.unit << "\n";
                    }
                }
            });
        } else if (trace->parsed()) {
            // A file, not a message: always over HTTP.
            const auto base = link.is_http() ? t.endpoint : endpoint_of("http");
            httplib::Client http(base);
            http.set_read_timeout(std::chrono::seconds(30));
            auto res = http.Get("/forge/api/trace?seconds=" + std::to_string(seconds));
            if (!res || res->status != 200) throw std::runtime_error("cannot read the trace from " + base);
            if (out_file.empty()) out_file = "trace.json";
            std::ofstream(out_file) << res->body;
            std::cout << "saved to " << out_file << " (open in https://ui.perfetto.dev)\n";
        } else if (audit->parsed()) {
            forge_audit_request req{};
            req.limit = limit;
            link.each<forge_audit_request, forge_audit_reply>(forge_msg_id_audit, "audit", req,
                                                              [&](const forge_audit_reply& page) {
                if (t.as_json) return print_json(page);
                for (pb_size_t k = 0; k < page.records_count; ++k) {
                    const auto& x = page.records[k];
                    std::cout << clock_time(x.time_unix_ns) << "  " << (x.user[0] ? std::string(x.user) + "@" : "")
                              << x.channel << " " << x.client << "  " << x.action << " " << x.target;
                    if (x.new_value[0]) std::cout << " = " << x.new_value;
                    if (!x.ok) std::cout << "  REFUSED: " << x.error;
                    std::cout << "\n";
                }
            });
        } else if (health->parsed()) {
            link.each<forge_health_request, forge_health_reply>(
                forge_msg_id_health, "health", {}, [&](const forge_health_reply& page) { print_json(page); });
        } else if (streams->parsed()) {
            link.each<forge_streams_request, forge_streams_reply>(
                forge_msg_id_streams, "streams", {}, [&](const forge_streams_reply& page) { print_json(page); });
        } else if (lease->parsed()) {
            forge_lease_request req{};
            set_text(req.id, id);
            req.seconds = lease_seconds;
            std::cout << id << " leased for "
                      << link.one<forge_lease_request, forge_lease_reply>(forge_msg_id_lease, "lease", req).seconds
                      << " s\n";
        } else if (release->parsed()) {
            forge_release_request req{};
            set_text(req.id, id);
            link.one<forge_release_request, forge_release_reply>(forge_msg_id_release, "release", req);
        } else if (log_level->parsed()) {
            forge_log_level_request req{};
            if (level.empty()) {
                set_text(req.level, logger_name);
            } else {
                set_text(req.logger, logger_name);
                set_text(req.level, level);
            }
            link.one<forge_log_level_request, forge_log_level_reply>(forge_msg_id_log_level, "log_level", req);
        } else if (tmtc->parsed()) {
            // Any command by name, in JSON: over HTTP, where the program knows the types.
            httplib::Client http(link.is_http() ? t.endpoint : endpoint_of("http"));
            http.set_read_timeout(std::chrono::milliseconds(t.timeout_ms));
            httplib::Headers h;
            if (!t.token.empty()) h.emplace("Authorization", "Bearer " + t.token);
            auto res = id.empty()
                           ? http.Get("/forge/api/tmtc", h)
                           : http.Post("/forge/api/tmtc/" + id, h, value.empty() ? "{}" : value, "application/json");
            if (!res) throw std::runtime_error("no answer over HTTP");
            const auto body = json::parse(res->body, nullptr, false);
            if (res->status != 200) {
                throw std::runtime_error("HTTP " + std::to_string(res->status) + ": " +
                                         (body.is_object() ? body.value("error", res->body) : res->body));
            }
            if (!id.empty() || t.as_json) {
                std::cout << body.dump(2) << "\n";
            } else {
                for (const auto& c : body) {
                    std::printf("0x%04x  %-16s %s%s\n", c["id"].get<unsigned>(), c["name"].get<std::string>().c_str(),
                                c["push"].get<bool>() ? "report " : "",
                                c["request"].is_string() ? c["request"].get<std::string>().c_str() : "");
                }
            }
        } else if (stream->parsed()) {
            httplib::Client http(link.is_http() ? t.endpoint : endpoint_of("http"));
            http.set_read_timeout(std::chrono::seconds(30));
            unsigned seen = 0;
            std::string pending;
            auto res = http.Get("/forge/api/streams/" + id + "/watch", [&](const char* data, std::size_t n) {
                pending.append(data, n);
                for (auto end = pending.find("\n\n"); end != std::string::npos; end = pending.find("\n\n")) {
                    const auto event = pending.substr(0, end);
                    pending.erase(0, end + 2);
                    const auto at = event.find("data: ");
                    if (event.find("event: message") == std::string::npos || at == std::string::npos) continue;
                    const auto m = json::parse(event.substr(at + 6), nullptr, false);
                    if (m.is_discarded()) continue;
                    if (t.as_json) {
                        std::cout << m.dump() << std::endl;
                    } else {
                        std::cout << clock_time(m["time_unix_ns"].get<std::int64_t>()) << "  #" << m["seq"] << "  "
                                  << m["body"].dump() << std::endl;
                    }
                    if (count && ++seen >= count) return false;
                }
                return !g_stop;
            });
            if (!res && seen == 0) throw std::runtime_error("cannot watch stream '" + id + "' over HTTP");
            if (res && res->status != 200)
                throw std::runtime_error("HTTP " + std::to_string(res->status) + ": " + res->body);
        } else if (watch->parsed()) {
            if (events_endpoint.empty()) events_endpoint = endpoint_of("events");
            if (topics.empty()) topics = {"state", "alarms", "log", "audit", "health"};
            forge::zmq::Receiver::Options o;
            o.topics = topics;
            std::cerr << "watching " << events_endpoint << " (Ctrl-C to stop)\n";
            forge::zmq::Receiver sub(forge::zmq::Receiver::Kind::sub, events_endpoint, [&](forge::zmq::Message&& m) {
                const auto when = clock_time(m.header.time_unix_ns);
                if (m.topic == "log") {
                    const auto x = m.as<forge_log_record>();
                    std::cout << when << " log   [" << level_text(x.level) << "] [" << x.logger << "] " << x.message
                              << "\n";
                } else if (m.topic == "state") {
                    const auto x = m.as<forge_state_report>();
                    std::cout << when << " state " << x.id << " = " << x.value << "\n";
                } else if (m.topic == "audit") {
                    const auto x = m.as<forge_audit_record>();
                    std::cout << when << " audit " << (x.user[0] ? std::string(x.user) + "@" : "") << x.channel << " "
                              << x.action << " " << x.target << (x.new_value[0] ? " = " + std::string(x.new_value) : "")
                              << (x.ok ? "" : " REFUSED: " + std::string(x.error)) << "\n";
                } else if (m.topic == "alarms") {
                    const auto a = m.as<forge_alarm>();
                    if (a.active && !a.acknowledged) print_alarm(when + " alarm ", a);
                } else if (m.topic == "health") {
                    std::cout << when << " health " << pb::to_json(m.as<forge_health_reply>()).dump() << "\n";
                } else if (m.topic == "metrics") {
                    std::cout << when << " metrics " << m.as<forge_metrics_reply>().metrics_count << " values\n";
                }
                std::cout.flush();
            }, o);
            while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    } catch (const std::exception& e) {
        std::cerr << "forgectl: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
