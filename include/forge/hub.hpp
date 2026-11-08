#pragma once
// Registration with forge-hub, the optional registry and dashboard of the
// applications on a device (see proto/forge/hub.proto and hub/).
//
// forge::Application does this by itself, configured by [hub]:
//
//   [hub]
//   register = true                    # try to register (nothing happens without a hub)
//   endpoint = "tcp://127.0.0.1:5550"
//   retry_s = 30                       # register again in the background (0 = try once)
//   critical = true                    # its unit failing is a critical alarm on the hub
//   group = "Radio"                    # dashboard grouping
//
// Restarting is systemd's job: the restart policy is in the application's unit
// (forge_add_app RESTART / MAX_RESTARTS / RESTART_WINDOW / WATCHDOG).
//
// Registration never blocks or fails the application: it runs on its own
// thread, and without a hub the application simply carries on.

#include "forge/config.hpp"
#include "forge/hub.forge.hpp"
#include "forge/status.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace forge::hub {

inline constexpr const char* default_endpoint = "tcp://127.0.0.1:5550";

/// systemd unit (".service") the process runs in, from /proc/<pid>/cgroup ("" if none).
std::string unit_of(int pid);
/// Instance of a template unit: "radio-controller@rx2.service" -> "rx2" ("" otherwise).
std::string instance_of(const std::string& unit);

/// Register with the hub at `endpoint` (the hub's "register" command).
Status register_app(const std::string& endpoint, const forge_hub_app_registration& app, forge_hub_register_reply& reply,
                    std::chrono::milliseconds timeout);
/// Tell the hub an application ends ("unregister").
Status unregister_app(const std::string& endpoint, const forge_hub_unregister_request& request,
                      std::chrono::milliseconds timeout);

/// Registers in the background: once right away, then every `retry` (0 = only
/// once). Thread-safe; stop() sends the unregistration if it ever registered.
class Client {
public:
    using Builder = std::function<forge_hub_app_registration()>;

    Client(std::string endpoint, std::chrono::seconds retry, Builder build,
           std::chrono::milliseconds timeout = std::chrono::milliseconds(500));
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void start();
    /// Stop retrying and tell the hub why the application ends.
    void stop(forge_hub_unregister_reason reason, int exit_code);

    bool registered() const { return registered_.load(); }
    const std::string& endpoint() const { return endpoint_; }

private:
    void run();

    std::string endpoint_;
    std::chrono::seconds retry_;
    Builder build_;
    std::chrono::milliseconds timeout_;
    std::atomic<bool> registered_{false}, ever_registered_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::thread thread_;
    forge_hub_app_registration last_{};
};

}  // namespace forge::hub
