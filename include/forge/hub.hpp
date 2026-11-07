#pragma once
// Registration with forge-hub, the optional registry and dashboard of the
// applications on a device (see proto/forge/v1/hub.proto and hub/).
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
#include "forge/v1/hub.pb.h"

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

/// One request/reply exchange with a hub; nullopt when it does not answer in time.
std::optional<v1::HubReply> exchange(const std::string& endpoint, const v1::HubRequest& request,
                                     std::chrono::milliseconds timeout);

/// Registers in the background: once right away, then every `retry` (0 = only
/// once). Thread-safe; stop() sends the unregistration if it ever registered.
class Client {
public:
    using Builder = std::function<v1::AppRegistration()>;

    Client(std::string endpoint, std::chrono::seconds retry, Builder build,
           std::chrono::milliseconds timeout = std::chrono::milliseconds(500));
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void start();
    /// Stop retrying and tell the hub why the application ends.
    void stop(v1::Unregister::Reason reason, int exit_code);

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
    v1::AppRegistration last_;
};

}  // namespace forge::hub
