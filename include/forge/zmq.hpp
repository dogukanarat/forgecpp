#pragma once
// ZeroMQ transport: streams and events (PUB/SUB), and c2 commands (request/reply).
//
// Every published message is multipart:
//
//   frame 0: topic   (UTF-8, used for PUB/SUB prefix filtering)
//   frame 1: header  (forge.stream_header: source, seq, time, body type, trace context)
//   frame 2: body    (the nanopb message named in header.type)
//   frame 3: blob    (optional raw bytes, e.g. IQ samples, described by the body)
//
// Commands are c2 messages (forge/c2.hpp), one per frame: the request is one
// frame, the answer one frame per part.
//
// Building blocks:
//   Publisher  PUB socket shared by many streams, thread-safe, with per-topic
//              subscriber counts, rate limits, "latest only" streams and
//              zero-copy blobs. Every application has two: [forge].events
//              (state, alarms, logs, audit, health, metrics) and [forge].data
//              (its own streams).
//   Sender     PUB or PUSH socket used from one thread
//   Receiver   SUB or PULL with its own thread; restores the trace context
//   Server     request/reply for REQ clients: ROUTER, an I/O thread and a pool of workers
//   Client     c2 command client (REQ) with timeout and reconnect
//
//   auto& spectrum = app.stream<sa_spectrum>("spectrum", {.max_rate_hz = 20});
//   if (spectrum.wanted()) spectrum.publish(frame);           // any thread
//
//   forge::zmq::Receiver sub(forge::zmq::Receiver::Kind::sub, "tcp://host:5558",
//       [](forge::zmq::Message&& m) { auto block = m.as<radio_iq_block>(); ... },
//       {.topics = {"rx2"}});

#include "forge/c2.hpp"
#include "forge/json.hpp"
#include "forge/messages.forge.hpp"
#include "forge/pb.hpp"
#include "forge/status.hpp"
#include "forge/subscription.hpp"
#include "forge/tmtc.hpp"

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
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace forge::zmq {

/// The shared ZeroMQ context.
::zmq::context_t& context();

/// Name of this program in stream headers and client names (Application sets it).
void set_source(std::string name);
std::string source();

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

/// A received (or to be sent) message.
struct Message {
    std::string topic;
    forge_stream_header header{};
    std::string body;  ///< encoded message of type header.type
    std::string blob;  ///< optional raw bytes

    /// Decode the body; throws Error if the type is another one or the bytes are wrong.
    template <pb::Message T>
    T as() const {
        const std::string_view want = pb::type_of<T>->name;
        const auto have = pb::view(header.type);
        if (!have.empty() && have != want) {
            throw Error("message on '" + topic + "' is " + std::string(have) + ", not " + std::string(want));
        }
        T out{};
        if (!pb::decode({reinterpret_cast<const std::uint8_t*>(body.data()), body.size()}, out).ok()) {
            throw Error("cannot decode " + std::string(want) + " on '" + topic + "'");
        }
        return out;
    }
};

/// A message with the header publish() would give it, for Sender::send()
/// (a test that changes the time, a relay that adds a field).
Message make_message(std::string_view topic, const pb::Type& type, const void* body, std::string blob = {});
template <pb::Message T>
Message make_message(std::string_view topic, const T& body, std::string blob = {}) {
    return make_message(topic, *pb::type_of<T>, &body, std::move(blob));
}

/// What a stream reports about itself (the "streams" command, the panel).
struct StreamInfo {
    std::string name, endpoint, topic, type, policy;
    double max_rate_hz = 0;
    std::uint64_t subscribers = 0, sent = 0, dropped = 0;
};
json to_json(const StreamInfo& i);

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
    const pb::Type& type() const { return type_; }

    /// True when someone subscribes and the rate limit allows a message now.
    /// Check it before doing the work of building a message.
    bool wanted();
    std::size_t subscribers() const { return subscribers_.load(); }

    /// Queue `body` (+ optional blob). Returns false if it was dropped
    /// (queue full). The blob is sent without copying while `owner` lives;
    /// with a null owner it is copied (the caller may reuse its buffer).
    template <pb::Message T>
    bool publish(const T& body, BlobOwner owner = {}, const void* data = nullptr, std::size_t size = 0) {
        return publish(*pb::type_of<T>, &body, std::move(owner), data, size);
    }
    /// `type` must be the stream's type (std::logic_error otherwise).
    bool publish(const pb::Type& type, const void* body, BlobOwner owner, const void* data, std::size_t size);
    /// Send a received message on, with its header (source, time, trace) but our sequence.
    bool forward(const Message& message);

    /// Look at the messages from inside the program (the panel's stream
    /// inspector): at most watch_rate_hz of them, without the blob. While
    /// someone watches, wanted() is true that often even without subscribers.
    /// `fn` runs on the publishing thread: keep it short.
    [[nodiscard]] Subscription watch(std::function<void(const Message&)> fn);
    static constexpr double watch_rate_hz = 5;

    StreamInfo info() const;

private:
    bool watch_due(bool take);

    friend class Publisher;
    Stream(Publisher& owner, std::string name, const pb::Type& type, StreamOptions options);
    bool enqueue(forge_stream_header header, std::string body, BlobOwner owner, const void* data, std::size_t size);

    Publisher& owner_;
    std::string name_;
    const pb::Type& type_;
    StreamOptions options_;
    std::atomic<std::uint64_t> seq_{0}, sent_{0}, dropped_{0};
    std::atomic<std::size_t> subscribers_{0};
    std::mutex rate_mutex_;
    std::chrono::steady_clock::time_point next_{};
    Listeners<Message> watchers_;
    std::shared_ptr<std::atomic<int>> watching_ = std::make_shared<std::atomic<int>>(0);
    std::atomic<std::int64_t> next_watch_ns_{0};
};

/// PUB socket with an I/O thread: thread-safe streams on one endpoint.
class Publisher {
public:
    Publisher(const std::string& endpoint, SocketOptions options = {});
    ~Publisher();
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    /// Create (or return) the stream `name` (topic) carrying `type` messages.
    Stream& add(const std::string& name, const pb::Type& type, StreamOptions options = {});
    template <pb::Message T>
    Stream& add(const std::string& name, StreamOptions options = {}) {
        return add(name, *pb::type_of<T>, std::move(options));
    }
    Stream* find(const std::string& name);
    std::vector<StreamInfo> list() const;
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
    template <pb::Message T>
    bool publish(std::string_view topic, const T& body, const void* blob = nullptr, std::size_t size = 0) {
        return publish(topic, *pb::type_of<T>, &body, blob, size);
    }
    bool publish(std::string_view topic, const pb::Type& type, const void* body, const void* blob, std::size_t size);
    /// Send a prepared message as is.
    bool send(const Message& message);
    /// Wait up to `timeout` until the socket takes a message (PUSH: a peer with
    /// room: backpressure), then send it. False after the timeout: nothing was
    /// sent and nothing counted as dropped, so a loop can wait again.
    bool send_wait(const Message& message, std::chrono::milliseconds timeout);

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
    Receiver(Kind kind, const std::string& endpoint, Handler handler, const Options& options);
    ~Receiver();

    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    void stop();
    std::uint64_t received() const { return received_; }
    /// Messages lost between sender and us (gaps in the header's seq, per source and topic).
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
/// not hold up the others; every answer goes back to the client that asked.
/// The handler gets the request frame and returns the answer frames; with
/// more than one worker it must be thread-safe.
struct ServerOptions {
    int workers = 1;
    /// Requests that waited longer than this for a worker are refused without
    /// being handled (their client has most likely given up).
    std::chrono::milliseconds max_queue_wait{std::chrono::seconds(5)};
    /// Requests beyond this many waiting ones are refused at once.
    std::size_t max_queued = 256;
    std::string thread_name = "forge-rep";
    /// The answer to a request that is refused because the server is busy
    /// (default: one empty frame).
    std::function<std::vector<std::string>(std::string_view request)> busy;
};

class Server {
public:
    using Handler = std::function<std::vector<std::string>(std::string_view request)>;

    Server(const std::string& endpoint, Handler handler, SocketOptions options = {});
    Server(const std::string& endpoint, Handler handler, ServerOptions server, SocketOptions options = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void stop();
    std::string endpoint() const { return endpoint_; }
    /// Requests received / refused because the server was too busy.
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
    void send_reply(::zmq::socket_t& out, const Job& job, const std::vector<std::string>& frames);
    std::vector<std::string> busy(std::string_view request) const;

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

/// c2 command client (REQ). After a timeout the socket is rebuilt so the
/// next request works once the server is back ("lazy pirate"). Not thread-safe.
///
///   forge::zmq::Client c("tcp://board:5557");
///   forge_info_reply info{};
///   if (auto s = c.call(forge_msg_id_info, forge_info_request{}, info); !s.ok()) ...
/// What `commands` answers to one c2 request, as Server frames:
///   Server s(endpoint, [&](std::string_view r) { return answer(commands, r); });
std::vector<std::string> answer(const tmtc::Registry& commands, std::string_view request);

class Client {
public:
    explicit Client(std::string endpoint, std::chrono::milliseconds timeout = std::chrono::seconds(2));

    void timeout(std::chrono::milliseconds t) { timeout_ = t; }
    const std::string& endpoint() const { return endpoint_; }

    /// One request; each answer (several when it comes in parts) goes to
    /// `on_answer`, which may stop the rest with an error. Returns the status
    /// of a NACK, ETIMEDOUT without an answer, or what on_answer returned.
    Status request(std::uint16_t id, std::span<const std::uint8_t> payload,
                   const std::function<Status(const c2::Message&)>& on_answer);

    /// A command with one reply (an ACK leaves `rsp` empty).
    template <pb::Message Req, pb::Message Rsp>
    Status call(std::uint16_t id, const Req& req, Rsp& rsp) {
        rsp = Rsp{};
        return call(id, *pb::type_of<Req>, &req,
                    [&](const c2::Message& m) { return m.payload.empty() ? Status() : pb::decode(m.payload, rsp); });
    }
    /// A command that answers in parts: `each` gets every part.
    template <pb::Message Req, pb::Message Rsp>
    Status call_parts(std::uint16_t id, const Req& req, const std::function<void(const Rsp&)>& each) {
        return call(id, *pb::type_of<Req>, &req, [&](const c2::Message& m) {
            Rsp rsp{};
            if (auto s = pb::decode(m.payload, rsp); !s.ok()) return s;
            each(rsp);
            return Status();
        });
    }

private:
    Status call(std::uint16_t id, const pb::Type& type, const void* req,
                const std::function<Status(const c2::Message&)>& on_answer);
    void reconnect();

    std::string endpoint_;
    std::chrono::milliseconds timeout_;
    std::uint16_t seq_ = 0;
    std::unique_ptr<::zmq::socket_t> socket_;
};

}  // namespace forge::zmq
