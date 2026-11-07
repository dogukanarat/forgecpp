#pragma once
// ControlService: the one path every control change takes, whatever the
// transport (web JSON API, forge.v1 over ZeroMQ or HTTP, console, telnet).
// It applies, in this order: trace context, authorization (roles), leases,
// the change itself on the main loop, audit, persistence and live updates.
//
//   forge.v1 Request -> handle(request, caller) -> Reply
//   typed calls (used by the JSON API and the consoles): set / press / call / ack ...
//
// Leases: a client can hold a control (or "group:<title>") for some seconds;
// changes by anyone else are refused with 409 and the holder's name, so two
// tools never fight over a setting. Leases expire on their own.

#include "forge/auth.hpp"
#include "forge/error.hpp"
#include "forge/json.hpp"
#include "forge/proto.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace forge {

class Application;

class ControlService {
public:
    explicit ControlService(Application& app);

    /// Resolve the caller's identity from a token (Request.token / bearer) or
    /// an HTTP Authorization header value; anonymous callers get the default role.
    Caller identify(std::string channel, std::string client, std::string_view token,
                    std::string_view http_authorization = {}) const;

    /// forge.v1 entry point (any thread).
    v1::Reply handle(const v1::Request& request, const Caller& caller);
    /// Serialized Request -> serialized Reply (transports).
    std::string handle_bytes(std::string_view request, const Caller& caller, std::string_view peer = {});
    /// Older JSON commands ({"cmd":"set","id":..,"value":..}) -> JSON reply.
    /// Deprecated: counted in forge.control.legacy_json, a warning once per
    /// client; refused (410) with [forge].legacy_json = false. Use forge.v1.
    json handle_legacy_json(const json& request, const Caller& caller);
    void set_legacy_json(bool enabled) { legacy_json_ = enabled; }

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

    /// Build the forge.v1 schema (controls with values, commands, leases).
    v1::Schema schema() const;

private:
    void require(Role needed, const Caller& caller, const std::string& what) const;
    void check_lease(const std::string& id, const Caller& caller);
    void audit(const Caller& caller, const std::string& action, const std::string& target, const json& old_value,
               const json& new_value, bool ok, const std::string& error);
    v1::Reply dispatch(const v1::Request& request, const Caller& caller);

    Application& app_;
    struct LeaseEntry {
        std::string holder;
        std::chrono::steady_clock::time_point expires;
    };
    mutable std::mutex lease_mutex_;
    std::map<std::string, LeaseEntry> leases_;
    std::atomic<bool> legacy_json_{true};
    std::mutex legacy_mutex_;
    std::set<std::string> legacy_clients_;  ///< already warned about
};

}  // namespace forge
