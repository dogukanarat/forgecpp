#pragma once
// ControlService: the one path every control change takes, whatever the
// transport (web JSON API, commands over a link, console, telnet). It applies,
// in this order: authorization (roles), leases, the change itself on the main
// loop, audit, persistence and live updates.
//
// A transport that runs a command for a caller sets that caller for the
// thread (CallerScope); the built-in commands read it with current_caller().
//
// Leases: a client can hold a control (or "group:<title>") for some seconds;
// changes by anyone else are refused with 409 and the holder's name, so two
// tools never fight over a setting. Leases expire on their own.

#include "forge/auth.hpp"
#include "forge/error.hpp"
#include "forge/json.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace forge {

class Application;

class ControlService {
public:
    explicit ControlService(Application& app);

    /// Resolve the caller's identity from a token (bearer) or an HTTP
    /// Authorization header value; anonymous callers get the default role.
    Caller identify(std::string channel, std::string client, std::string_view token,
                    std::string_view http_authorization = {}) const;

    // --- typed calls; throw forge::Error(status, message) --------------------------
    json get(const std::string& id, const Caller& caller);
    json set(const std::string& id, const json& value, const Caller& caller);
    void press(const std::string& id, const Caller& caller);
    json call(const std::string& id, const json& args, const Caller& caller);
    std::size_t ack(const std::string& id, const Caller& caller);
    /// Lease `id` (control id or "group:<title>") for `seconds` (0 = default 60).
    std::chrono::seconds lease(const std::string& id, unsigned seconds, const Caller& caller);
    void release(const std::string& id, const Caller& caller);
    void set_log_level(const std::string& logger, const std::string& level, const Caller& caller);

    /// Holder of a lease covering control `id` (empty: none).
    std::string lease_holder(const std::string& id) const;
    /// Active leases: [{"id", "holder", "seconds_left"}].
    json leases() const;

private:
    void require(Role needed, const Caller& caller, const std::string& what) const;
    void check_lease(const std::string& id, const Caller& caller);
    void audit(const Caller& caller, const std::string& action, const std::string& target, const json& old_value,
               const json& new_value, bool ok, const std::string& error);

    Application& app_;
    struct LeaseEntry {
        std::string holder;
        std::chrono::steady_clock::time_point expires;
    };
    mutable std::mutex lease_mutex_;
    std::map<std::string, LeaseEntry> leases_;
};

/// The caller of the command this thread runs (an internal admin when none is set).
const Caller& current_caller();

/// Sets current_caller() for a scope.
class CallerScope {
public:
    explicit CallerScope(Caller caller);
    ~CallerScope();
    CallerScope(const CallerScope&) = delete;
    CallerScope& operator=(const CallerScope&) = delete;

private:
    Caller caller_;
    const Caller* previous_;
};

}  // namespace forge
