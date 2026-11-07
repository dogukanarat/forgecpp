#include "forge/control.hpp"

#include "forge/application.hpp"
#include "forge/http.hpp"
#include "forge/metrics.hpp"
#include "forge/trace.hpp"
#include "forge/version.hpp"
#include "forge/web.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

namespace forge {
namespace {

using http::HttpError;

const char* call_name(v1::Request::KindCase c) {
    switch (c) {
        case v1::Request::kPing: return "rpc.ping";
        case v1::Request::kInfo: return "rpc.info";
        case v1::Request::kSchema: return "rpc.schema";
        case v1::Request::kState: return "rpc.state";
        case v1::Request::kGet: return "rpc.get";
        case v1::Request::kSet: return "rpc.set";
        case v1::Request::kPress: return "rpc.press";
        case v1::Request::kCall: return "rpc.call";
        case v1::Request::kAlarms: return "rpc.alarms";
        case v1::Request::kAck: return "rpc.ack";
        case v1::Request::kLogs: return "rpc.logs";
        case v1::Request::kMetrics: return "rpc.metrics";
        case v1::Request::kTraceDump: return "rpc.trace";
        case v1::Request::kAudit: return "rpc.audit";
        case v1::Request::kLease: return "rpc.lease";
        case v1::Request::kRelease: return "rpc.release";
        case v1::Request::kHealth: return "rpc.health";
        case v1::Request::kStreams: return "rpc.streams";
        case v1::Request::kLogLevel: return "rpc.log_level";
        default: return "rpc.unknown";
    }
}

v1::Alarm to_proto(const Alarm& a) {
    auto ms = [](Alarm::Clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    };
    v1::Alarm p;
    p.set_id(a.id);
    p.set_severity(proto::to_proto(static_cast<int>(a.severity)));
    p.set_message(a.message);
    p.set_active(a.active);
    p.set_acknowledged(a.acknowledged);
    p.set_acknowledged_by(a.acknowledged_by);
    p.set_count(a.count);
    p.set_raised_unix_ms(ms(a.raised_at));
    p.set_last_unix_ms(ms(a.last_at));
    if (a.cleared_at) p.set_cleared_unix_ms(ms(*a.cleared_at));
    if (a.acknowledged_at) p.set_acknowledged_unix_ms(ms(*a.acknowledged_at));
    return p;
}

}  // namespace

ControlService::ControlService(Application& app) : app_(app) {}

Caller ControlService::identify(std::string channel, std::string client, std::string_view token,
                                std::string_view http_authorization) const {
    Caller c;
    c.channel = std::move(channel);
    c.client = std::move(client);
    const auto& auth = app_.auth();
    c.role = auth.default_role();
    std::optional<Auth::User> user;
    if (!token.empty()) user = auth.by_token(token);
    if (!user && !http_authorization.empty()) user = auth.by_http_header(http_authorization);
    if (user) {
        c.user = user->name;
        c.role = user->role;
    }
    return c;
}

void ControlService::require(Role needed, const Caller& caller, const std::string& what) const {
    if (static_cast<int>(caller.role) >= static_cast<int>(needed)) return;
    throw HttpError(caller.user.empty() && app_.auth().enabled() ? 401 : 403,
                    fmt::format("{} needs role {} ({} is {})", what, to_string(needed), caller.describe(),
                                to_string(caller.role)));
}

json ControlService::leases() const {
    std::lock_guard lock(lease_mutex_);
    const auto now = std::chrono::steady_clock::now();
    json out = json::array();
    for (const auto& [id, e] : leases_) {
        if (e.expires <= now) continue;
        out.push_back({{"id", id},
                       {"holder", e.holder},
                       {"seconds_left", std::chrono::duration_cast<std::chrono::seconds>(e.expires - now).count()}});
    }
    return out;
}

std::string ControlService::lease_holder(const std::string& id) const {
    std::lock_guard lock(lease_mutex_);
    const auto now = std::chrono::steady_clock::now();
    auto holder_of = [&](const std::string& key) -> std::string {
        auto it = leases_.find(key);
        return (it != leases_.end() && it->second.expires > now) ? it->second.holder : std::string();
    };
    if (auto h = holder_of(id); !h.empty()) return h;
    auto* d = app_.dashboard();
    if (d && d->contains(id)) return holder_of("group:" + d->control(id).group);
    return {};
}

void ControlService::check_lease(const std::string& id, const Caller& caller) {
    const auto holder = lease_holder(id);
    if (!holder.empty() && holder != caller.describe()) {
        throw HttpError(409, fmt::format("'{}' is leased by {}", id, holder));
    }
}

void ControlService::audit(const Caller& caller, const std::string& action, const std::string& target,
                           const json& old_value, const json& new_value, bool ok, const std::string& error) {
    v1::AuditRecord r;
    r.set_channel(caller.channel);
    r.set_client(caller.client);
    r.set_user(caller.user);
    r.set_action(action);
    r.set_target(target);
    if (!old_value.is_null()) *r.mutable_old_value() = proto::to_value(old_value);
    if (!new_value.is_null()) *r.mutable_new_value() = proto::to_value(new_value);
    r.set_ok(ok);
    r.set_error(error);
    const auto ctx = trace::current();
    if (ctx.valid()) *r.mutable_trace() = trace::to_proto(ctx);
    app_.audit().record(std::move(r));
    if (ok) {
        app_.logger()->info("{}: {} {}{}", caller.describe(), action, target,
                            new_value.is_null() ? std::string() : " = " + new_value.dump());
    } else {
        app_.logger()->warn("{}: {} {} refused: {}", caller.describe(), action, target, error);
    }
}

json ControlService::get(const std::string& id, const Caller& caller) {
    require(Role::viewer, caller, "reading '" + id + "'");
    return app_.invoke_for(app_.request_timeout(), [&] { return app_.dashboard()->get(id); });
}

json ControlService::set(const std::string& id, const json& value, const Caller& caller) {
    json old;
    try {
        auto* d = app_.dashboard();
        const auto& c = app_.invoke_for(app_.request_timeout(), [&]() -> const web::detail::ControlDef& { return d->control(id); });
        if (c.kind == web::Kind::button) {
            press(id, caller);
            return nullptr;
        }
        require(c.role, caller, "changing '" + id + "'");
        check_lease(id, caller);
        json now;
        app_.invoke_for(app_.request_timeout(), [&] {
            old = d->get(id);
            d->set(id, value);
            now = d->get(id);
        });
        audit(caller, "set", id, old, now, true, {});
        app_.control_changed(id, c.persist);
        return now;
    } catch (const HttpError& e) {
        audit(caller, "set", id, old, value, false, e.what());
        throw;
    } catch (const std::exception& e) {
        audit(caller, "set", id, old, value, false, e.what());
        throw HttpError(500, e.what());
    }
}

void ControlService::press(const std::string& id, const Caller& caller) {
    try {
        auto* d = app_.dashboard();
        const auto& c = app_.invoke_for(app_.request_timeout(), [&]() -> const web::detail::ControlDef& { return d->control(id); });
        require(c.role, caller, "pressing '" + id + "'");
        check_lease(id, caller);
        app_.invoke_for(app_.request_timeout(), [&] { d->press(id); });
        audit(caller, "press", id, nullptr, nullptr, true, {});
        app_.control_changed(id, false);
    } catch (const HttpError& e) {
        audit(caller, "press", id, nullptr, nullptr, false, e.what());
        throw;
    } catch (const std::exception& e) {
        audit(caller, "press", id, nullptr, nullptr, false, e.what());
        throw HttpError(500, e.what());
    }
}

json ControlService::call(const std::string& id, const json& args, const Caller& caller) {
    try {
        auto* d = app_.dashboard();
        const auto role = app_.invoke_for(app_.request_timeout(), [&] { return d->command(id).role; });
        require(role, caller, "calling '" + id + "'");
        check_lease(id, caller);
        json result = app_.invoke_for(app_.request_timeout(), [&] { return d->call(id, args); });
        audit(caller, "call", id, nullptr, args, true, {});
        app_.control_changed(id, false);
        return result;
    } catch (const HttpError& e) {
        audit(caller, "call", id, nullptr, args, false, e.what());
        throw;
    } catch (const std::exception& e) {
        audit(caller, "call", id, nullptr, args, false, e.what());
        throw HttpError(500, e.what());
    }
}

std::size_t ControlService::ack(const std::string& id, const Caller& caller) {
    require(Role::operator_, caller, "acknowledging alarms");
    std::size_t n = 0;
    if (id == "all") {
        n = app_.alarms().acknowledge_all(caller.describe());
    } else if (app_.alarms().acknowledge(id, caller.describe())) {
        n = 1;
    } else {
        audit(caller, "ack", id, nullptr, nullptr, false, "unknown alarm");
        throw HttpError(404, "unknown alarm '" + id + "'");
    }
    audit(caller, "ack", id, nullptr, nullptr, true, {});
    return n;
}

std::chrono::seconds ControlService::lease(const std::string& id, unsigned seconds, const Caller& caller) {
    require(Role::operator_, caller, "leasing '" + id + "'");
    auto* d = app_.dashboard();
    const bool group = id.rfind("group:", 0) == 0;
    if (!group && !app_.invoke_for(app_.request_timeout(), [&] { return d->contains(id) || d->has_command(id); })) {
        throw HttpError(404, "unknown control '" + id + "'");
    }
    const auto duration = std::chrono::seconds(seconds == 0 ? 60 : std::min(seconds, 3600u));
    {
        std::lock_guard lock(lease_mutex_);
        auto& l = leases_[id];
        const auto now = std::chrono::steady_clock::now();
        if (!l.holder.empty() && l.expires > now && l.holder != caller.describe()) {
            throw HttpError(409, fmt::format("'{}' is leased by {}", id, l.holder));
        }
        l.holder = caller.describe();
        l.expires = now + duration;
    }
    audit(caller, "lease", id, nullptr, static_cast<std::int64_t>(duration.count()), true, {});
    return duration;
}

void ControlService::release(const std::string& id, const Caller& caller) {
    require(Role::operator_, caller, "releasing '" + id + "'");
    {
        std::lock_guard lock(lease_mutex_);
        auto it = leases_.find(id);
        if (it == leases_.end()) return;
        if (it->second.holder != caller.describe() && caller.role != Role::admin) {
            throw HttpError(409, fmt::format("'{}' is leased by {}", id, it->second.holder));
        }
        leases_.erase(it);
    }
    audit(caller, "release", id, nullptr, nullptr, true, {});
}

void ControlService::set_log_level(const std::string& logger, const std::string& level, const Caller& caller) {
    require(Role::admin, caller, "changing log levels");
    try {
        if (logger.empty()) {
            log::set_level(level);
        } else {
            log::set_logger_level(logger, level);
        }
    } catch (const std::exception& e) {
        throw HttpError(400, e.what());
    }
    audit(caller, "log-level", logger.empty() ? "*" : logger, nullptr, level, true, {});
}

v1::Schema ControlService::schema() const {
    return app_.invoke_for(app_.request_timeout(), [this] {
        v1::Schema s;
        const auto j = app_.dashboard()->schema();
        s.set_title(j.value("title", std::string()));
        for (const auto& g : j["groups"]) {
            const auto group = g["title"].get<std::string>();
            for (const auto& c : g["controls"]) {
                auto* p = s.add_controls();
                p->set_id(c["id"].get<std::string>());
                p->set_group(group);
                p->set_label(c.value("label", std::string()));
                p->set_kind(c.value("kind", std::string()));
                if (c.contains("value")) *p->mutable_value() = proto::to_value(c["value"]);
                p->set_unit(c.value("unit", std::string()));
                p->set_help(c.value("help", std::string()));
                if (c.contains("min") || c.contains("max")) {
                    p->set_has_range(true);
                    p->set_min(c.value("min", 0.0));
                    p->set_max(c.value("max", 0.0));
                }
                p->set_step(c.value("step", 0.0));
                p->set_precision(c.value("precision", -1));
                for (const auto& o : c.value("options", json::array())) p->add_options(o.get<std::string>());
                p->set_readonly(c.value("readonly", false) || c["kind"] == "value");
                p->set_role(c.value("role", std::string("operator")));
                p->set_persistent(c.value("persist", false));
                p->set_lease_holder(lease_holder(p->id()));
            }
            for (const auto& c : g.value("commands", json::array())) {
                auto* p = s.add_commands();
                p->set_id(c["id"].get<std::string>());
                p->set_label(c.value("label", std::string()));
                p->set_help(c.value("help", std::string()));
                p->set_role(c.value("role", std::string("operator")));
                for (const auto& pa : c["params"]) {
                    auto* q = p->add_params();
                    q->set_name(pa["name"].get<std::string>());
                    q->set_type(pa.value("type", std::string()));
                    q->set_help(pa.value("help", std::string()));
                    if (pa.contains("default")) *q->mutable_default_value() = proto::to_value(pa["default"]);
                    for (const auto& o : pa.value("options", json::array())) q->add_options(o.get<std::string>());
                    q->set_required(!pa.contains("default"));
                }
            }
        }
        return s;
    });
}

v1::Reply ControlService::handle(const v1::Request& request, const Caller& caller) {
    // Continue the caller's trace (or start one): spans, logs and the audit
    // record of this request all carry its trace id.
    const auto parent = request.has_trace() ? trace::from_proto(request.trace()) : trace::Context{};
    trace::ContextScope scope(trace::child_of(parent));
    static auto& requests = metrics::registry().counter("forge.rpc.requests", "Control requests handled");
    static auto& failed = metrics::registry().counter("forge.rpc.errors", "Control requests that failed");
    static auto& seconds =
        metrics::registry().histogram("forge.rpc.seconds", metrics::latency_buckets(), "Control request duration", "s");
    requests.inc();
    metrics::Timer timer(seconds);
    trace::Scope span(call_name(request.kind_case()), "rpc");

    v1::Reply reply;
    try {
        reply = dispatch(request, caller);
        reply.set_ok(true);
        if (reply.status() == 0) reply.set_status(200);
    } catch (const HttpError& e) {
        reply = proto::error_reply(e.status(), e.what());
    } catch (const std::exception& e) {
        reply = proto::error_reply(500, e.what());
    }
    if (!reply.ok()) failed.inc();
    *reply.mutable_trace() = trace::to_proto(trace::current());
    return reply;
}

v1::Reply ControlService::dispatch(const v1::Request& r, const Caller& caller) {
    v1::Reply out;
    switch (r.kind_case()) {
        case v1::Request::kPing:
            break;
        case v1::Request::kInfo: {
            require(Role::viewer, caller, "info");
            auto* i = out.mutable_info();
            const auto& info = app_.info();
            i->set_name(info.name);
            i->set_version(info.version);
            i->set_description(info.description);
            i->set_forgecpp(version_string);
            i->set_host(proto::host());
            i->set_pid(static_cast<std::uint32_t>(::getpid()));
            i->set_uptime_s(std::chrono::duration_cast<std::chrono::seconds>(app_.uptime()).count());
            for (const auto& [k, v] : app_.endpoints()) (*i->mutable_endpoints())[k] = v;
            i->set_protocol(proto::kProtocol);
            break;
        }
        case v1::Request::kSchema:
            require(Role::viewer, caller, "schema");
            *out.mutable_schema() = schema();
            break;
        case v1::Request::kState: {
            require(Role::viewer, caller, "state");
            const auto st = app_.invoke_for(app_.request_timeout(), [this] { return app_.dashboard()->state(); });
            auto* v = out.mutable_state()->mutable_values();
            for (const auto& [k, val] : st.items()) (*v)[k] = proto::to_value(val);
            break;
        }
        case v1::Request::kGet:
            *out.mutable_value() = proto::to_value(get(r.get().id(), caller));
            break;
        case v1::Request::kSet:
            *out.mutable_value() = proto::to_value(set(r.set().id(), proto::to_json(r.set().value()), caller));
            break;
        case v1::Request::kPress:
            press(r.press().id(), caller);
            break;
        case v1::Request::kCall:
            *out.mutable_value() = proto::to_value(call(r.call().id(), proto::to_json(r.call().args()), caller));
            break;
        case v1::Request::kAlarms:
            require(Role::viewer, caller, "alarms");
            for (const auto& a : app_.alarms().list()) *out.mutable_alarms()->add_alarms() = to_proto(a);
            break;
        case v1::Request::kAck:
            out.set_count(static_cast<std::uint32_t>(ack(r.ack().id(), caller)));
            break;
        case v1::Request::kLogs:
            require(Role::viewer, caller, "logs");
            for (auto& rec : log::history(r.logs().limit())) *out.mutable_logs()->add_records() = std::move(rec);
            break;
        case v1::Request::kMetrics:
            require(Role::viewer, caller, "metrics");
            *out.mutable_metrics() = metrics::registry().snapshot(r.metrics().prefix());
            break;
        case v1::Request::kTraceDump: {
            require(Role::viewer, caller, "trace");
            auto d = trace::dump(r.trace_dump().seconds());
            auto* t = out.mutable_trace_dump();
            t->set_chrome_json(std::move(d.chrome_json));
            t->set_events(d.events);
            t->set_dropped(d.dropped);
            break;
        }
        case v1::Request::kAudit:
            require(Role::viewer, caller, "audit");
            for (auto& rec : app_.audit().list(r.audit().limit())) *out.mutable_audit()->add_records() = std::move(rec);
            break;
        case v1::Request::kLease:
            out.set_count(static_cast<std::uint32_t>(lease(r.lease().id(), r.lease().seconds(), caller).count()));
            break;
        case v1::Request::kRelease:
            release(r.release().id(), caller);
            break;
        case v1::Request::kHealth:
            require(Role::viewer, caller, "health");
            *out.mutable_health() = app_.health().snapshot();
            break;
        case v1::Request::kStreams:
            require(Role::viewer, caller, "streams");
            *out.mutable_streams() = app_.stream_list();
            break;
        case v1::Request::kLogLevel:
            set_log_level(r.log_level().logger(), r.log_level().level(), caller);
            break;
        default:
            throw HttpError(400, "empty or unknown request");
    }
    return out;
}

std::string ControlService::handle_bytes(std::string_view bytes, const Caller& transport, std::string_view peer) {
    v1::Request req;
    v1::Reply reply;
    if (!req.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        reply = proto::error_reply(400, "not a forge.v1.Request");
    } else {
        Caller c = identify(transport.channel, req.client().empty() ? std::string(peer) : req.client(), req.token());
        if (!transport.user.empty()) {  // already authenticated by the transport
            c.user = transport.user;
            c.role = transport.role;
        }
        reply = handle(req, c);
    }
    std::string out;
    reply.SerializeToString(&out);
    return out;
}

json ControlService::handle_legacy_json(const json& req, const Caller& transport) {
    try {
        const auto cmd = req.value("cmd", std::string());
        Caller caller = identify(transport.channel, req.value("client", transport.client),
                                 req.value("token", std::string()));
        if (!transport.user.empty()) {
            caller.user = transport.user;
            caller.role = transport.role;
        }
        auto need_id = [&] {
            if (!req.contains("id") || !req["id"].is_string()) throw HttpError(400, "\"id\" is required");
            return req["id"].get<std::string>();
        };
        if (cmd == "ping") return {{"ok", true}, {"pong", true}};
        if (cmd == "info") {
            v1::Request r;
            r.mutable_info();
            auto reply = handle(r, caller);
            auto j = proto::to_json(reply.info());
            j["ok"] = true;
            j["alarms"] = app_.alarms().summary();
            return j;
        }
        if (cmd == "ui") {
            require(Role::viewer, caller, "ui");
            return {{"ok", true}, {"ui", app_.invoke_for(app_.request_timeout(), [this] { return app_.dashboard()->schema(); })}};
        }
        if (cmd == "state") {
            require(Role::viewer, caller, "state");
            return {{"ok", true}, {"state", app_.invoke_for(app_.request_timeout(), [this] { return app_.dashboard()->state(); })}};
        }
        if (cmd == "get") {
            const auto id = need_id();
            return {{"ok", true}, {"id", id}, {"value", get(id, caller)}};
        }
        if (cmd == "set") {
            const auto id = need_id();
            if (!req.contains("value")) throw HttpError(400, "\"value\" is required");
            return {{"ok", true}, {"id", id}, {"value", set(id, req["value"], caller)}};
        }
        if (cmd == "press") {
            const auto id = need_id();
            press(id, caller);
            return {{"ok", true}, {"id", id}};
        }
        if (cmd == "call") {
            const auto id = need_id();
            return {{"ok", true}, {"id", id}, {"result", call(id, req.value("args", json::object()), caller)}};
        }
        if (cmd == "alarms") {
            require(Role::viewer, caller, "alarms");
            return {{"ok", true}, {"alarms", app_.alarms().to_json()}};
        }
        if (cmd == "ack") return {{"ok", true}, {"acknowledged", ack(need_id(), caller)}};
        throw HttpError(400, "unknown command '" + cmd + "' (ping|info|ui|state|get|set|press|call|alarms|ack)");
    } catch (const HttpError& e) {
        return {{"ok", false}, {"error", e.what()}, {"status", e.status()}};
    } catch (const std::exception& e) {
        return {{"ok", false}, {"error", e.what()}, {"status", 500}};
    }
}

}  // namespace forge
