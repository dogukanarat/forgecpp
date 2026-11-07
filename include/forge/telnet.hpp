#pragma once
// Remote console over telnet: the application's console commands from any
// machine, with the same commands as `-i` (controls, set, press, call,
// alarms, metrics, trace, ...), plus:
//   logs on [level] | logs off   follow the application log in this session
//   whoami                       user and role of this session
//   exit                         close the session (the application keeps running)
//
//   [console]
//   telnet = "0.0.0.0:2323"      # empty = off
//   telnet_max_sessions = 4
//   telnet_idle_s = 0            # close idle sessions after this many seconds (0 = never)
//
// With [auth] enabled the session asks for a user name and password
// ([auth.users.<name>]) and gets that user's role; otherwise every session
// is admin, like the local console. Telnet is plain text: use it on trusted
// networks or through an SSH tunnel. `nc host 2323` works too.

#include "forge/auth.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace forge {

class Console;

class Telnet {
public:
    struct Options {
        std::string listen = "0.0.0.0:2323";
        std::size_t max_sessions = 4;
        unsigned idle_s = 0;
        std::string banner;
        std::string prompt = "> ";
    };

    Telnet(Console& console, const Auth& auth, Options options);
    ~Telnet();
    Telnet(const Telnet&) = delete;
    Telnet& operator=(const Telnet&) = delete;

    /// Bind and start accepting. Throws std::runtime_error when the address is unusable.
    void start();
    void stop();
    /// "host:port" actually bound (port resolved when 0 was asked).
    std::string endpoint() const { return endpoint_; }
    std::size_t sessions() const;

private:
    struct Session;
    void accept_loop();
    void serve(std::shared_ptr<Session> s);

    Console& console_;
    const Auth& auth_;
    Options options_;
    int listen_fd_ = -1;
    std::string endpoint_;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    mutable std::mutex mutex_;
    std::list<std::shared_ptr<Session>> sessions_;
};

}  // namespace forge
