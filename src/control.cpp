#include "forge/control.hpp"

#include "forge/application.hpp"
#include "forge/trace.hpp"
#include "forge/web.hpp"

#include <format>

#include <algorithm>

namespace forge {
namespace {

thread_local const Caller* tls_caller = nullptr;

}  // namespace

const Caller& current_caller() {
    static const Caller internal;  // channel "internal", role admin
    return tls_caller ? *tls_caller : internal;
}

CallerScope::CallerScope(Caller caller) : caller_(std::move(caller)), previous_(tls_caller) { tls_caller = &caller_; }
CallerScope::~CallerScope() { tls_caller = previous_; }

ControlService::ControlService(Application& app, std::function<void(bool)> changed)
    : app_(app), changed_(std::move(changed)) {}

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

Status ControlService::run_command(const std::string& name, Role role, const std::function<json()>& request,
                                   const std::function<Status()>& run, const Caller& caller) {
    const bool audited = role != Role::viewer;  // a query changes nothing: no record, like reading a control
    try {
        require(role, caller, "command '" + name + "'");
        const Status s = run();
        if (audited) audit(caller, "command", name, nullptr, request(), s.ok(), s.ok() ? "" : s.to_string());
        return s;
    } catch (const Error& e) {
        if (audited) audit(caller, "command", name, nullptr, request(), false, e.what());
        throw;
    }
}

void ControlService::require(Role needed, const Caller& caller, const std::string& what) const {
    if (static_cast<int>(caller.role) >= static_cast<int>(needed)) return;
    throw Error(
        caller.user.empty() && app_.auth().enabled() ? 401 : 403,
        std::format("{} needs role {} ({} is {})", what, to_string(needed), caller.describe(), to_string(caller.role)))
        .module(module_id::tmtc);  // forgecpp refuses, not the command
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
        throw Error(409, std::format("'{}' is leased by {}", id, holder));
    }
}

void ControlService::audit(const Caller& caller, const std::string& action, const std::string& target,
                           const json& old_value, const json& new_value, bool ok, const std::string& error) {
    Audit::Record r;
    r.channel = caller.channel;
    r.client = caller.client;
    r.user = caller.user;
    r.action = action;
    r.target = target;
    r.old_value = old_value;
    r.new_value = new_value;
    r.ok = ok;
    r.error = error;
    r.trace = trace::current();
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
        const auto& c =
            app_.invoke_for(app_.request_timeout(), [&]() -> const web::detail::ControlDef& { return d->control(id); });
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
        if (changed_) changed_(c.persist);
        return now;
    } catch (const Error& e) {
        audit(caller, "set", id, old, value, false, e.what());
        throw;
    } catch (const std::exception& e) {
        audit(caller, "set", id, old, value, false, e.what());
        throw Error(500, e.what());
    }
}

void ControlService::press(const std::string& id, const Caller& caller) {
    try {
        auto* d = app_.dashboard();
        const auto& c =
            app_.invoke_for(app_.request_timeout(), [&]() -> const web::detail::ControlDef& { return d->control(id); });
        require(c.role, caller, "pressing '" + id + "'");
        check_lease(id, caller);
        app_.invoke_for(app_.request_timeout(), [&] { d->press(id); });
        audit(caller, "press", id, nullptr, nullptr, true, {});
        if (changed_) changed_(false);
    } catch (const Error& e) {
        audit(caller, "press", id, nullptr, nullptr, false, e.what());
        throw;
    } catch (const std::exception& e) {
        audit(caller, "press", id, nullptr, nullptr, false, e.what());
        throw Error(500, e.what());
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
        if (changed_) changed_(false);
        return result;
    } catch (const Error& e) {
        audit(caller, "call", id, nullptr, args, false, e.what());
        throw;
    } catch (const std::exception& e) {
        audit(caller, "call", id, nullptr, args, false, e.what());
        throw Error(500, e.what());
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
        throw Error(404, "unknown alarm '" + id + "'");
    }
    audit(caller, "ack", id, nullptr, nullptr, true, {});
    return n;
}

std::chrono::seconds ControlService::lease(const std::string& id, unsigned seconds, const Caller& caller) {
    require(Role::operator_, caller, "leasing '" + id + "'");
    auto* d = app_.dashboard();
    const bool group = id.rfind("group:", 0) == 0;
    if (!group && !app_.invoke_for(app_.request_timeout(), [&] { return d->contains(id) || d->has_command(id); })) {
        throw Error(404, "unknown control '" + id + "'");
    }
    const auto duration = std::chrono::seconds(seconds == 0 ? 60 : std::min(seconds, 3600u));
    {
        std::lock_guard lock(lease_mutex_);
        auto& l = leases_[id];
        const auto now = std::chrono::steady_clock::now();
        if (!l.holder.empty() && l.expires > now && l.holder != caller.describe()) {
            throw Error(409, std::format("'{}' is leased by {}", id, l.holder));
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
            throw Error(409, std::format("'{}' is leased by {}", id, it->second.holder));
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
        throw Error(400, e.what());
    }
    audit(caller, "log-level", logger.empty() ? "*" : logger, nullptr, level, true, {});
}

}  // namespace forge
