#pragma once
// ZeroMQ transport of the forge.v1 message standard (libzmq + cppzmq).
//
// Every published message is multipart (see proto/forge/v1/common.proto):
//
//   frame 0: topic   (UTF-8, used for PUB/SUB prefix filtering)
//   frame 1: forge.v1.Header (source, seq, time, body type, trace context)
//   frame 2: body    (the protobuf message named in Header.type)
//   frame 3: blob    (optional raw bytes, e.g. IQ samples, described by the body)
//
// Control requests and replies are single frames: forge.v1.Request / Reply
// (a REP socket also accepts the older JSON commands, see ControlService).
//
// Building blocks:
//   Publisher  PUB socket shared by many streams, thread-safe, with per-topic
//              subscriber counts, rate limits, "latest only" streams and
//              zero-copy blobs. Every application has two: [forge].events
//              (state, alarms, logs, audit, health, metrics) and [forge].data
//              (its own streams).
//   Sender     PUB or PUSH socket used from one thread
//   Receiver   SUB or PULL with its own thread; restores the trace context
//   Server     request/reply (REP) with its own thread
//   Client     forge.v1 control client (REQ) with timeout and reconnect
//
//   auto& spectrum = app.streams().add("spectrum", "sa.v1.Spectrum", {.max_rate_hz = 20});
//   if (spectrum.wanted()) spectrum.publish(frame);           // any thread
//
//   forge::zmq::Receiver sub(forge::zmq::Receiver::Kind::sub, "tcp://host:5558",
//       [](forge::zmq::Message&& m) { auto block = m.as<radio::v1::IqBlock>(); ... },
//       {.topics = {"rx2"}});

#include "forge/json.hpp"
#include "forge/v1/common.pb.h"
#include "forge/v1/control.pb.h"

#include <google/protobuf/message.h>
#include <zmq.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace forge {
class Application;
class ControlService;
}  // namespace forge

namespace forge::zmq {

/// Process-wide ZeroMQ context (one I/O thread).
::zmq::context_t& context();

/// libzmq version, e.g. "4.3.5".
std::string version();

/// Errors from Client (timeouts, transport) and remote errors returned by servers.
class Error : public std::runtime_error {
public:
    explicit Error(const std::string& what, int status = 0) : std::runtime_error(what), status_(status) {}
    /// HTTP-like status for remote errors (400, 403, 404, ...), 0 for transport errors.
    int status() const { return status_; }

private:
    int status_;
};

/// A received (or to be sent) forge.v1 message.
struct Message {
    std::string topic;
    v1::Header header;
    std::string body;  ///< serialized message of type header.type()
    std::string blob;  ///< optional raw bytes

    /// Parse the body; throws Error if the type does not match or parsing fails.
    template <class T>
    T as() const {
        T out;
        if (!header.type().empty() && header.type() != out.GetTypeName()) {
            throw Error("message on '" + topic + "' is " + header.type() + ", not " + out.GetTypeName());
        }
        if (!out.ParseFromString(body)) throw Error("cannot parse " + out.GetTypeName() + " on '" + topic + "'");
        return out;
    }
};

struct SocketOptions {
    int send_hwm = 1000;   ///< messages queued per peer before dropping / blocking
    int recv_hwm = 1000;
    int linger_ms = 0;     ///< how long unsent messages are kept after close
};

/// Keeps blob memory alive until ZeroMQ has sent it (zero copy).
using BlobOwner = std::shared_ptr<const void>;

// ---------------------------------------------------------------------------
// Publisher and streams

struct StreamOptions {
    /// all: every message is queued; latest: only the newest unsent one is
    /// kept (slow consumers see fresh data, never a backlog).
    std::string policy = "all";
    /// Maximum messages per second (0 = unlimited); wanted() is false in between.
    double max_rate_hz = 0;
};

class Publisher;

/// One topic of a Publisher. publish() is thread-safe.
class Stream {
public:
    const std::string& name() const { return name_; }
    const std::string& type() const { return type_; }

    /// True when someone subscribes and the rate limit allows a message now.
    /// Check it before doing the work of building a message.
    bool wanted();
    std::size_t subscribers() const { return subscribers_.load(); }

    /// Queue `body` (+ optional blob). Returns false if it was dropped
    /// (queue full). The blob is sent without copying while `owner` lives;
    /// with a null owner it is copied (the caller may reuse its buffer).
    bool publish(const google::protobuf::Message& body);
    bool publish(const google::protobuf::Message& body, BlobOwner owner, const void* data, std::size_t size);
    /// Same with an explicit header (e.g. to forward a received message).
    bool publish_with_header(v1::Header header, std::string body, BlobOwner owner, const void* data,
                             std::size_t size);

    v1::StreamInfo info() const;

private:
    friend class Publisher;
    Stream(Publisher& owner, std::string name, std::string type, StreamOptions options);

    Publisher& owner_;
    std::string name_, type_;
    StreamOptions options_;
    std::atomic<std::uint64_t> seq_{0}, sent_{0}, dropped_{0};
    std::atomic<std::size_t> subscribers_{0};
    std::mutex rate_mutex_;
    std::chrono::steady_clock::time_point next_{};
};

/// PUB socket with an I/O thread: thread-safe streams on one endpoint.
class Publisher {
public:
    Publisher(const std::string& endpoint, SocketOptions options = {});
    ~Publisher();
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    /// Create (or return) the stream `name` (topic) carrying `type` messages.
    Stream& add(const std::string& name, const std::string& type, StreamOptions options = {});
    Stream* find(const std::string& name);
    std::vector<v1::StreamInfo> list() const;
    std::string endpoint() const { return endpoint_; }
    /// Messages waiting for the I/O thread.
    std::size_t queued() const;
    void stop();

private:
    friend class Stream;
    struct Outgoing {
        Stream* stream = nullptr;
        std::string header, body;
        BlobOwner owner;
        const void* data = nullptr;
        std::size_t size = 0;
    };
    bool enqueue(Stream& s, Outgoing out);
    void run();
    void update_subscriptions(std::string_view prefix, bool subscribe);

    std::string endpoint_;
    SocketOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Outgoing> queue_;
    std::map<std::string, Outgoing> latest_;  // per stream, policy "latest"
    std::map<std::string, std::unique_ptr<Stream>> streams_;
    std::map<std::string, std::size_t> prefixes_;  // subscription prefix -> count
    std::atomic<bool> running_{true};
    std::thread thread_;
    std::promise<std::string> bound_;
};

// ---------------------------------------------------------------------------
// Single-thread sockets

/// PUB or PUSH sender. Not thread-safe: use it from one thread (or use Publisher).
class Sender {
public:
    enum class Kind { pub, push };

    /// `bind` binds the endpoint ("tcp://*:5555"), otherwise connects to it.
    Sender(Kind kind, const std::string& endpoint, bool bind = true, SocketOptions options = {});

    /// Send `body` with a standard header (seq per topic, type = body type).
    /// Non-blocking: returns false when dropped (PUSH with no peer / full).
    bool publish(std::string_view topic, const google::protobuf::Message& body, const void* blob = nullptr,
                 std::size_t size = 0);
    /// Send a prepared message as is.
    bool send(const Message& message);

    /// PUB only: active subscriptions (each subscriber counts once per topic).
    std::size_t subscriptions();
    std::uint64_t sent() const { return sent_; }
    std::uint64_t dropped() const { return dropped_; }
    /// Resolved endpoint (useful with "tcp://*:0" or "tcp://127.0.0.1:*").
    std::string endpoint() const;
    ::zmq::socket_t& native() { return socket_; }

private:
    bool send_frames(std::string_view topic, const std::string& header, const std::string& body, const void* blob,
                     std::size_t size);
    void poll_subscriptions();

    Kind kind_;
    ::zmq::socket_t socket_;
    std::int64_t subscriptions_ = 0;
    std::uint64_t sent_ = 0;
    std::uint64_t dropped_ = 0;
    std::map<std::string, std::uint64_t, std::less<>> seq_;
};

/// SUB or PULL receiver running its own thread. The handler is called on
/// that thread with the sender's trace context; hand work to the main loop
/// with Application::post().
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
    /// Messages lost between sender and us (gaps in Header.seq, per source and topic).
    std::uint64_t lost() const { return lost_; }
    std::string endpoint() const { return endpoint_; }

private:
    void run();

    ::zmq::socket_t socket_;
    Handler handler_;
    std::string endpoint_;
    std::atomic<bool> running_{true};
    std::atomic<std::uint64_t> received_{0}, lost_{0};
    std::map<std::string, std::uint64_t> last_seq_;
    std::thread thread_;
};

/// Request/reply server for REQ (and DEALER) clients. A ROUTER socket on its
/// own I/O thread hands requests to `workers` threads, so a slow request does
/// not hold up the others; every reply goes back to the client that asked.
/// The handler gets the raw request frame and returns the raw reply frame; with
/// more than one worker it must be thread-safe.
struct ServerOptions {
    int workers = 1;
    /// Requests that waited longer than this for a worker are answered 503
    /// without being handled (their client has most likely given up).
    std::chrono::milliseconds max_queue_wait{std::chrono::seconds(5)};
    /// Requests beyond this many waiting ones are answered 503 at once.
    std::size_t max_queued = 256;
    std::string thread_name = "forge-rep";
};

class Server {
public:
    using Handler = std::function<std::string(std::string_view request)>;

    Server(const std::string& endpoint, Handler handler, SocketOptions options = {});
    Server(const std::string& endpoint, Handler handler, ServerOptions server, SocketOptions options = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void stop();
    std::string endpoint() const { return endpoint_; }
    /// Requests received / answered 503 because the server was too busy.
    std::uint64_t requests() const { return requests_; }
    std::uint64_t rejected() const { return rejected_; }
    int workers() const { return static_cast<int>(workers_.size()); }

private:
    struct Job {
        std::vector<std::string> envelope;  ///< routing frames up to the empty delimiter
        std::string body;
        std::chrono::steady_clock::time_point received;
    };
    void run();
    void work();
    void send_reply(::zmq::socket_t& out, const Job& job, const std::string& reply);

    ::zmq::socket_t socket_;
    Handler handler_;
    ServerOptions server_options_;
    std::string endpoint_, replies_endpoint_;
    std::atomic<bool> running_{true};
    std::atomic<std::uint64_t> requests_{0}, rejected_{0};

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    bool stopping_ = false;
    ::zmq::socket_t replies_;  ///< PULL: workers' replies to the I/O thread
    std::vector<std::thread> workers_;
    std::thread thread_;
};

/// forge.v1 control client (REQ). After a timeout the socket is rebuilt so
/// the next request works once the server is back ("lazy pirate").
/// Not thread-safe.
class Client {
public:
    explicit Client(std::string endpoint, std::chrono::milliseconds timeout = std::chrono::seconds(2));

    /// Identity and credentials sent with every request.
    void set_client(std::string name) { client_ = std::move(name); }
    void set_token(std::string token) { token_ = std::move(token); }
    void timeout(std::chrono::milliseconds t) { timeout_ = t; }
    const std::string& endpoint() const { return endpoint_; }

    /// Send a request (client, token and trace context are filled in) and
    /// wait for the reply. Throws Error on timeout; remote errors are in the reply.
    v1::Reply request(v1::Request request);
    /// Same, but throws Error(status) when the reply is not ok.
    v1::Reply call(v1::Request request);

    // Shortcuts (throw Error on failure)
    json get(const std::string& id);
    json set(const std::string& id, const json& value);
    void press(const std::string& id);
    json call_command(const std::string& id, const json& args = json::object());
    v1::Info info();

    /// Older JSON commands ({"cmd": ...}); kept for scripts written for them.
    json request_json(const json& request);

private:
    std::string exchange(const std::string& bytes);
    void reconnect();

    std::string endpoint_;
    std::chrono::milliseconds timeout_;
    std::string client_, token_;
    std::unique_ptr<::zmq::socket_t> socket_;
};

/// Serve a ControlService (REQ clients): forge.v1 Request frames, and the
/// older JSON commands (a frame starting with '{'). Applications do this
/// themselves for [forge].control; call it for additional endpoints.
/// `workers` requests are handled at the same time (ControlService is thread-safe).
std::unique_ptr<Server> serve_control(ControlService& service, const std::string& endpoint, int workers = 2);

/// Compatibility: serve_control(app.control(), endpoint).
std::unique_ptr<Server> serve_application(Application& app, const std::string& endpoint);

}  // namespace forge::zmq
