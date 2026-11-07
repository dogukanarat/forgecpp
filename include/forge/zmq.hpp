#pragma once
// ZeroMQ messaging for forgecpp daemons (libzmq + cppzmq).
//
// Link forgecpp::zmq. Every message on the wire is a multipart message:
//
//   frame 0: topic   (UTF-8, used for PUB/SUB prefix filtering)
//   frame 1: header  (JSON object)
//   frame 2: payload (optional raw bytes, e.g. IQ samples)
//
// Building blocks:
//   Sender    PUB (fan-out, slow subscribers drop) or PUSH (load-balanced, lossless)
//   Receiver  SUB or PULL with its own thread
//   Server    JSON request/reply (REP) with its own thread
//   Client    JSON request/reply (REQ) with timeout and automatic reconnect
//   serve_application()  exposes an Application's controls and alarms over ZeroMQ
//
//   forge::zmq::Sender pub(forge::zmq::Sender::Kind::pub, "tcp://*:5555");
//   pub.send("temperature", {{"value", 21.5}});
//
//   forge::zmq::Receiver::Options opts;
//   opts.topics = {"temperature"};
//   forge::zmq::Receiver sub(forge::zmq::Receiver::Kind::sub, "tcp://host:5555",
//                            [](forge::zmq::Message&& m) { ... }, opts);
//
// Sockets are not thread-safe: a Sender belongs to the thread that uses it;
// Receiver and Server own their socket on their internal thread.

#include "forge/json.hpp"

#include <zmq.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace forge {
class Application;
}

namespace forge::zmq {

/// Process-wide ZeroMQ context (one I/O thread).
::zmq::context_t& context();

/// libzmq version, e.g. "4.3.5".
std::string version();

struct Message {
    std::string topic;
    json header = json::object();
    std::string payload;  ///< raw bytes; empty when the message has no payload frame
};

/// Errors from Client (timeouts, transport) and remote errors returned by servers.
class Error : public std::runtime_error {
public:
    explicit Error(const std::string& what, int status = 0) : std::runtime_error(what), status_(status) {}
    /// HTTP-like status for remote errors (400, 403, 404, ...), 0 for transport errors.
    int status() const { return status_; }

private:
    int status_;
};

struct SocketOptions {
    int send_hwm = 1000;   ///< messages queued per peer before dropping / blocking
    int recv_hwm = 1000;
    int linger_ms = 0;     ///< how long unsent messages are kept after close
};

/// PUB or PUSH sender. Not thread-safe: use it from one thread.
class Sender {
public:
    enum class Kind { pub, push };

    /// `bind` binds the endpoint ("tcp://*:5555"), otherwise connects to it.
    Sender(Kind kind, const std::string& endpoint, bool bind = true, SocketOptions options = {});

    /// Non-blocking send. Returns false when the message was dropped:
    /// PUSH with no peer or all peers full. (PUB drops per slow subscriber
    /// silently, by design, so a slow client never stalls the others.)
    bool send(std::string_view topic, const json& header, const void* data = nullptr, std::size_t size = 0);
    bool send(const Message& message);

    /// PUB only: active subscriptions (each subscriber counts once per topic).
    std::size_t subscriptions();

    std::uint64_t sent() const { return sent_; }
    std::uint64_t dropped() const { return dropped_; }
    /// Resolved endpoint (useful with "tcp://*:0" or "tcp://127.0.0.1:*").
    std::string endpoint() const;
    ::zmq::socket_t& native() { return socket_; }

private:
    void poll_subscriptions();

    Kind kind_;
    ::zmq::socket_t socket_;
    std::int64_t subscriptions_ = 0;
    std::uint64_t sent_ = 0;
    std::uint64_t dropped_ = 0;
};

/// SUB or PULL receiver running its own thread. The handler is called on
/// that thread; hand work to the main loop with Application::post().
class Receiver {
public:
    enum class Kind { sub, pull };
    using Handler = std::function<void(Message&& message)>;

    struct Options {
        bool bind = false;                       ///< default: connect
        std::vector<std::string> topics{""};     ///< SUB prefixes ("" = everything)
        SocketOptions socket{};
    };

    Receiver(Kind kind, const std::string& endpoint, Handler handler);
    Receiver(Kind kind, const std::string& endpoint, Handler handler, Options options);
    ~Receiver();

    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    void stop();
    std::uint64_t received() const { return received_; }
    std::string endpoint() const { return endpoint_; }

private:
    void run();

    ::zmq::socket_t socket_;
    Handler handler_;
    std::string endpoint_;
    std::atomic<bool> running_{true};
    std::atomic<std::uint64_t> received_{0};
    std::thread thread_;
};

/// JSON request/reply server (REP) with its own thread. The handler's
/// exceptions become {"ok": false, "error": ..., "status": ...} replies.
class Server {
public:
    using Handler = std::function<json(const json& request)>;

    Server(const std::string& endpoint, Handler handler, SocketOptions options = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void stop();
    std::string endpoint() const { return endpoint_; }
    std::uint64_t requests() const { return requests_; }

private:
    void run();

    ::zmq::socket_t socket_;
    Handler handler_;
    std::string endpoint_;
    std::atomic<bool> running_{true};
    std::atomic<std::uint64_t> requests_{0};
    std::thread thread_;
};

/// JSON request/reply client (REQ). After a timeout the socket is rebuilt
/// so the next request works once the server is back ("lazy pirate").
/// Not thread-safe.
class Client {
public:
    explicit Client(std::string endpoint, std::chrono::milliseconds timeout = std::chrono::seconds(2));

    /// Send a request and wait for the raw reply. Throws Error on timeout.
    json request(const json& request);

    /// {"cmd": cmd, ...args} -> reply; throws Error(status) when ok == false.
    json call(const std::string& cmd, const json& args = json::object());

    void timeout(std::chrono::milliseconds t) { timeout_ = t; }
    const std::string& endpoint() const { return endpoint_; }

private:
    void reconnect();

    std::string endpoint_;
    std::chrono::milliseconds timeout_;
    std::unique_ptr<::zmq::socket_t> socket_;
};

/// Serve an Application's controls and alarms over ZeroMQ, next to the web UI
/// and the console. Requests are JSON objects with a "cmd":
///
///   {"cmd":"ping"}                           -> {"ok":true,"pong":true}
///   {"cmd":"info"}                           -> name, version, uptime, ...
///   {"cmd":"ui"}                             -> control descriptions + values
///   {"cmd":"state"}                          -> {"state":{"id":value,...}}
///   {"cmd":"get","id":"rx_lo"}               -> {"value":...}
///   {"cmd":"set","id":"rx_lo","value":2.4e9} -> {"value":...}  (validated)
///   {"cmd":"press","id":"calibrate"}
///   {"cmd":"alarms"}                         -> {"alarms":[...]}
///   {"cmd":"ack","id":"..."|"all"}
///
/// Replies carry "ok": true, or "ok": false with "error" and "status".
/// Commands run on the application's main loop. Call it from on_start().
std::unique_ptr<Server> serve_application(Application& app, const std::string& endpoint);

}  // namespace forge::zmq
