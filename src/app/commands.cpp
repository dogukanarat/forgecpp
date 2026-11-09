// The commands every application answers (proto/forge/messages.proto,
// numbers 0x0001..0x00FF), on top of ControlService and the other services.
#include "services.hpp"

#include "forge/control.hpp"
#include "forge/version.hpp"
#include "forge/web.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <unistd.h>

namespace forge::detail {
namespace {

// Text into a bounded field. These answers are listings for people
// (messages.proto says so): a longer text is cut, not refused.
template <std::size_t N>
void put(char (&dst)[N], std::string_view src) {
    const auto n = std::min(src.size(), N - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

std::string text(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

json parse(const char* field, std::string_view value) {
    if (value.empty()) return nullptr;
    auto j = json::parse(value, nullptr, false);
    if (j.is_discarded()) throw Error(400, std::string(field) + ": not JSON");
    return j;
}

std::int64_t unix_ms(std::chrono::system_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

// The typed member of a get or set reply (tags 2, 3, 4), when the value has one.
template <class Reply>
void put_typed(Reply& r, const json& v) {
    if (v.is_boolean()) {
        r.which_typed = 3;
        r.typed.flag = v.get<bool>();
    } else if (v.is_number()) {
        r.which_typed = 2;
        r.typed.number = v.get<double>();
    } else if (v.is_string() && v.get_ref<const std::string&>().size() < sizeof(r.typed.text)) {
        r.which_typed = 4;
        put(r.typed.text, v.get_ref<const std::string&>());
    }
}

static_assert(forge_get_reply_number_tag == 2 && forge_get_reply_flag_tag == 3 && forge_get_reply_text_tag == 4);
static_assert(forge_set_reply_number_tag == 2 && forge_set_reply_flag_tag == 3 && forge_set_reply_text_tag == 4);

json value_of(const forge_set_request& r) {
    switch (r.which_typed) {
        case forge_set_request_number_tag: return r.typed.number;
        case forge_set_request_flag_tag: return r.typed.flag;
        case forge_set_request_text_tag: return std::string(pb::view(r.typed.text));
        default: return parse("value", r.value);
    }
}

void put_trace(forge_trace_context& dst, bool& has, const trace::Context& ctx) {
    has = ctx.valid();
    if (!has) return;
    std::copy(ctx.trace_id.begin(), ctx.trace_id.end(), dst.trace_id);
    std::copy(ctx.span_id.begin(), ctx.span_id.end(), dst.span_id);
}

// One page of `items` per call: `more` is where the next page starts (0: none left).
template <class Item, class Fill>
void page(const std::vector<Item>& items, std::size_t per_page, std::uint32_t& more, Fill fill) {
    const std::size_t from = more;
    const std::size_t to = std::min(items.size(), from + per_page);
    for (std::size_t i = from; i < to; ++i) fill(items[i], i - from);
    more = to < items.size() ? static_cast<std::uint32_t>(to) : 0;
}

forge_severity severity_of(Severity s) { return static_cast<forge_severity>(static_cast<int>(s) + 1); }

}  // namespace

void fill(forge_alarm& out, const Alarm& a) {
    put(out.id, a.id);
    out.severity = severity_of(a.severity);
    put(out.message, a.message);
    out.active = a.active;
    out.acknowledged = a.acknowledged;
    put(out.acknowledged_by, a.acknowledged_by);
    out.count = a.count;
    out.raised_unix_ms = unix_ms(a.raised_at);
    out.last_unix_ms = unix_ms(a.last_at);
    out.cleared_unix_ms = a.cleared_at ? unix_ms(*a.cleared_at) : 0;
    out.acknowledged_unix_ms = a.acknowledged_at ? unix_ms(*a.acknowledged_at) : 0;
}

void fill(forge_log_record& out, const log::Record& r) {
    out.time_unix_ns = r.time_unix_ns;
    put(out.level, log::to_string(r.level));
    put(out.logger, r.logger);
    put(out.message, r.message);
    out.thread = r.thread;
    put_trace(out.trace, out.has_trace, r.trace);
}

void fill(forge_audit_record& out, const Audit::Record& r) {
    out.time_unix_ns = r.time_unix_ns;
    put(out.channel, r.channel);
    put(out.client, r.client);
    put(out.user, r.user);
    put(out.action, r.action);
    put(out.target, r.target);
    if (!r.old_value.is_null()) put(out.old_value, text(r.old_value));
    if (!r.new_value.is_null()) put(out.new_value, text(r.new_value));
    out.ok = r.ok;
    put(out.error, r.error);
    put_trace(out.trace, out.has_trace, r.trace);
}

void fill(forge_health_reply& out, const Health::Report& h, std::size_t from) {
    out.state = static_cast<forge_health_state>(h.state);
    const auto to = std::min(h.components.size(), from + std::size(out.components));
    for (std::size_t i = from; i < to; ++i) {
        const auto& c = h.components[i];
        auto& o = out.components[out.components_count++];
        put(o.name, c.name);
        o.state = static_cast<forge_health_state>(c.state);
        put(o.message, c.message);
        o.critical = c.critical;
        o.since_unix_ms = c.since_unix_ms;
    }
}

void register_builtin_commands(Application& app) {
    auto& r = app.commands();
    auto& control = app.control();
    using namespace std::chrono;

    r.add<forge_ping_request, forge_ping_reply>(forge_msg_id_ping, "ping", [&app](const auto& req, auto& rsp) {
        rsp.token = req.token;
        rsp.uptime_ms = static_cast<std::uint64_t>(duration_cast<milliseconds>(app.uptime()).count());
        return Status();
    });

    r.add<forge_info_request, forge_info_reply>(forge_msg_id_info, "info", [&app](const auto&, auto& rsp) {
        const auto& info = app.info();
        put(rsp.name, app.id());
        put(rsp.version, info.version);
        put(rsp.description, info.description);
        put(rsp.forgecpp, version_string);
        char host[64] = {};
        ::gethostname(host, sizeof(host) - 1);
        put(rsp.host, host);
        rsp.pid = static_cast<std::uint32_t>(::getpid());
        rsp.uptime_s = duration_cast<seconds>(app.uptime()).count();
        for (const auto& [name, url] : app.endpoints()) {
            if (rsp.endpoints_count == std::size(rsp.endpoints)) break;
            auto& e = rsp.endpoints[rsp.endpoints_count++];
            put(e.name, name);
            put(e.url, url);
        }
        return Status();
    });

    r.add_parts<forge_controls_request, forge_controls_reply>(
        forge_msg_id_controls, "controls", [&app, &control](const auto&, auto& rsp, std::uint32_t& more) {
        if (!app.dashboard()) return Status();
        const auto schema = app.invoke_for(app.request_timeout(), [&] { return app.dashboard()->schema(); });
        put(rsp.title, schema.value("title", std::string()));
        std::vector<std::pair<std::string, json>> controls;  // group, control
        for (const auto& g : schema["groups"]) {
            for (const auto& c : g["controls"]) controls.emplace_back(g.value("title", std::string()), c);
        }
        page(controls, std::size(rsp.controls), more, [&](const std::pair<std::string, json>& item, std::size_t) {
            const auto& [group, c] = item;
            auto& p = rsp.controls[rsp.controls_count++];
            put(p.id, c.value("id", std::string()));
            put(p.group, group);
            put(p.label, c.value("label", std::string()));
            put(p.kind, c.value("kind", std::string()));
            if (c.contains("value")) put(p.value, text(c["value"]));
            put(p.unit, c.value("unit", std::string()));
            put(p.help, c.value("help", std::string()));
            p.has_range = c.contains("min") || c.contains("max");
            p.min = c.value("min", 0.0);
            p.max = c.value("max", 0.0);
            p.step = c.value("step", 0.0);
            p.precision = c.value("precision", -1);
            for (const auto& o : c.value("options", json::array())) {
                if (p.options_count == std::size(p.options)) break;
                put(p.options[p.options_count++], o.get<std::string>());
            }
            p.readonly = c.value("readonly", false) || c.value("kind", std::string()) == "value";
            put(p.role, c.value("role", std::string("operator")));
            p.persistent = c.value("persist", false);
            put(p.lease_holder, control.lease_holder(c.value("id", std::string())));
        });
        return Status();
    });

    r.add<forge_get_request, forge_get_reply>(forge_msg_id_get, "get", [&control](const auto& req, auto& rsp) {
        const auto v = control.get(req.id, current_caller());
        put(rsp.value, text(v));
        put_typed(rsp, v);
        return Status();
    });

    r.add<forge_set_request, forge_set_reply>(forge_msg_id_set, "set", [&control](const auto& req, auto& rsp) {
        const auto v = control.set(req.id, value_of(req), current_caller());
        put(rsp.value, text(v));
        put_typed(rsp, v);
        return Status();
    });

    r.add<forge_press_request, forge_press_reply>(forge_msg_id_press, "press", [&control](const auto& req, auto&) {
        control.press(req.id, current_caller());
        return Status();
    });

    r.add<forge_call_request, forge_call_reply>(forge_msg_id_call, "call", [&control](const auto& req, auto& rsp) {
        auto args = parse("args", req.args);
        if (args.is_null()) args = json::object();
        const auto result = text(control.call(req.id, args, current_caller()));
        if (result.size() >= sizeof(rsp.result)) {
            return Status::error(module_id::tmtc, EMSGSIZE, "the result does not fit the reply");
        }
        put(rsp.result, result);
        return Status();
    });

    r.add_parts<forge_commands_request, forge_commands_reply>(forge_msg_id_commands, "commands",
                                                              [&app](const auto&, auto& rsp, std::uint32_t& more) {
        if (!app.dashboard()) return Status();
        const auto schema = app.invoke_for(app.request_timeout(), [&] { return app.dashboard()->schema(); });
        std::vector<json> commands;
        for (const auto& g : schema["groups"]) {
            for (const auto& c : g.value("commands", json::array())) commands.push_back(c);
        }
        page(commands, std::size(rsp.commands), more, [&](const json& c, std::size_t) {
            auto& p = rsp.commands[rsp.commands_count++];
            put(p.id, c.value("id", std::string()));
            put(p.label, c.value("label", std::string()));
            put(p.help, c.value("help", std::string()));
            put(p.role, c.value("role", std::string("operator")));
            for (const auto& pa : c.value("params", json::array())) {
                if (p.params_count == std::size(p.params)) break;
                auto& q = p.params[p.params_count++];
                put(q.name, pa.value("name", std::string()));
                put(q.type, pa.value("type", std::string()));
                put(q.help, pa.value("help", std::string()));
                if (pa.contains("default")) put(q.default_value, text(pa["default"]));
                for (const auto& o : pa.value("options", json::array())) {
                    if (q.options_count == std::size(q.options)) break;
                    put(q.options[q.options_count++], o.get<std::string>());
                }
                q.required = !pa.contains("default");
            }
        });
        return Status();
    });

    r.add_parts<forge_alarms_request, forge_alarms_reply>(forge_msg_id_alarms, "alarms",
                                                          [&app](const auto&, auto& rsp, std::uint32_t& more) {
        page(app.alarms().list(), std::size(rsp.alarms), more,
             [&](const Alarm& a, std::size_t) { fill(rsp.alarms[rsp.alarms_count++], a); });
        return Status();
    });

    r.add<forge_ack_request, forge_ack_reply>(forge_msg_id_ack, "ack", [&control](const auto& req, auto& rsp) {
        rsp.count = static_cast<std::uint32_t>(control.ack(req.id, current_caller()));
        return Status();
    });

    r.add_parts<forge_logs_request, forge_logs_reply>(forge_msg_id_logs, "logs",
                                                      [](const auto& req, auto& rsp, std::uint32_t& more) {
        page(log::history(req.limit), std::size(rsp.records), more,
             [&](const log::Record& rec, std::size_t) { fill(rsp.records[rsp.records_count++], rec); });
        return Status();
    });

    r.add_parts<forge_metrics_request, forge_metrics_reply>(forge_msg_id_metrics, "metrics",
                                                            [](const auto& req, auto& rsp, std::uint32_t& more) {
        const auto snap = metrics::registry().snapshot(req.prefix);
        rsp.time_unix_ns = snap.time_unix_ns;
        page(snap.metrics, std::size(rsp.metrics), more, [&](const metrics::Sample& m, std::size_t) {
            auto& o = rsp.metrics[rsp.metrics_count++];
            put(o.name, m.name);
            put(o.help, m.help);
            o.type = static_cast<forge_metric_type>(static_cast<int>(m.type) + 1);
            put(o.unit, m.unit);
            o.value = m.value;
            o.bounds_count = static_cast<pb_size_t>(std::min(m.bounds.size(), std::size(o.bounds)));
            std::copy_n(m.bounds.begin(), o.bounds_count, o.bounds);
            o.buckets_count = static_cast<pb_size_t>(std::min(m.buckets.size(), std::size(o.buckets)));
            std::copy_n(m.buckets.begin(), o.buckets_count, o.buckets);
            o.count = m.count;
            o.sum = m.sum;
            for (const auto& [k, v] : m.labels) {
                if (o.labels_count == std::size(o.labels)) break;
                auto& l = o.labels[o.labels_count++];
                put(l.key, k);
                put(l.value, v);
            }
        });
        return Status();
    });

    r.add_parts<forge_audit_request, forge_audit_reply>(forge_msg_id_audit, "audit",
                                                        [&app](const auto& req, auto& rsp, std::uint32_t& more) {
        page(app.audit().list(req.limit), std::size(rsp.records), more,
             [&](const Audit::Record& rec, std::size_t) { fill(rsp.records[rsp.records_count++], rec); });
        return Status();
    });

    r.add<forge_lease_request, forge_lease_reply>(forge_msg_id_lease, "lease", [&control](const auto& req, auto& rsp) {
        rsp.seconds = static_cast<std::uint32_t>(control.lease(req.id, req.seconds, current_caller()).count());
        return Status();
    });

    r.add<forge_release_request, forge_release_reply>(forge_msg_id_release, "release",
                                                      [&control](const auto& req, auto&) {
        control.release(req.id, current_caller());
        return Status();
    });

    r.add_parts<forge_health_request, forge_health_reply>(forge_msg_id_health, "health",
                                                          [&app](const auto&, auto& rsp, std::uint32_t& more) {
        const auto h = app.health().snapshot();
        const std::size_t from = more;
        fill(rsp, h, from);
        more = from + std::size(rsp.components) < h.components.size()
                   ? static_cast<std::uint32_t>(from + std::size(rsp.components))
                   : 0;
        return Status();
    });

    r.add_parts<forge_streams_request, forge_streams_reply>(forge_msg_id_streams, "streams",
                                                            [&app](const auto&, auto& rsp, std::uint32_t& more) {
        page(app.stream_list(), std::size(rsp.streams), more, [&](const zmq::StreamInfo& i, std::size_t) {
            auto& o = rsp.streams[rsp.streams_count++];
            put(o.name, i.name);
            put(o.endpoint, i.endpoint);
            put(o.topic, i.topic);
            put(o.type, i.type);
            put(o.policy, i.policy);
            o.max_rate_hz = i.max_rate_hz;
            o.subscribers = i.subscribers;
            o.sent = i.sent;
            o.dropped = i.dropped;
        });
        return Status();
    });

    r.add<forge_log_level_request, forge_log_level_reply>(forge_msg_id_log_level, "log_level",
                                                          [&control](const auto& req, auto&) {
        control.set_log_level(req.logger, req.level, current_caller());
        return Status();
    });

    r.add_push<forge_state_report>(forge_msg_id_state_changed, "state_changed");
    r.add_push<forge_alarm>(forge_msg_id_alarm_changed, "alarm_changed");
    r.add_push<forge_log_record>(forge_msg_id_log_written, "log_written");
    r.add_push<forge_audit_record>(forge_msg_id_audit_written, "audit_written");
    r.add_push<forge_health_reply>(forge_msg_id_health_changed, "health_changed");
}

}  // namespace forge::detail
