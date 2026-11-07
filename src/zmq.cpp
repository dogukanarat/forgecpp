#include "forge/zmq.hpp"

#include "forge/application.hpp"
#include "forge/control.hpp"
#include "forge/log.hpp"
#include "forge/proto.hpp"
#include "forge/trace.hpp"

#include <zmq_addon.hpp>

#include <unistd.h>

namespace forge::zmq {
namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(100);

void apply(::zmq::socket_t& s, const SocketOptions& o) {
    s.set(::zmq::sockopt::sndhwm, o.send_hwm);
    s.set(::zmq::sockopt::rcvhwm, o.recv_hwm);
    s.set(::zmq::sockopt::linger, o.linger_ms);
}

std::string resolved_endpoint(::zmq::socket_t& s, const std::string& fallback) {
    try {
        auto e = s.get(::zmq::sockopt::last_endpoint);
        return e.empty() ? fallback : e;
    } catch (const ::zmq::error_t&) {
        return fallback;
    }
}

// Receive one multipart forge.v1 message. Returns false on no message.
bool receive(::zmq::socket_t& s, Message& out) {
    std::vector<::zmq::message_t> parts;
    if (!::zmq::recv_multipart(s, std::back_inserter(parts), ::zmq::recv_flags::dontwait)) return false;
    out = Message{};
    if (!parts.empty()) out.topic = parts[0].to_string();
    if (parts.size() > 1 && parts[1].size() > 0) {
        if (!out.header.ParseFromArray(parts[1].data(), static_cast<int>(parts[1].size()))) out.header.Clear();
    }
    if (parts.size() > 2) out.body.assign(static_cast<const char*>(parts[2].data()), parts[2].size());
    if (parts.size() > 3) out.blob.assign(static_cast<const char*>(parts[3].data()), parts[3].size());
    return true;
}

void free_owner(void*, void* hint) { delete static_cast<BlobOwner*>(hint); }

// Send [topic][header][body][blob?]; the blob without copying when owned.
bool send_parts(::zmq::socket_t& s, std::string_view topic, const std::string& header, const std::string& body,
                const BlobOwner& owner, const void* data, std::size_t size, bool dontwait) {
    const bool blob = data != nullptr && size > 0;
    const auto first = dontwait ? (::zmq::send_flags::sndmore | ::zmq::send_flags::dontwait) : ::zmq::send_flags::sndmore;
    // The first frame decides: if it is accepted, the rest follow atomically.
    if (!s.send(::zmq::buffer(topic.data(), topic.size()), first)) return false;
    s.send(::zmq::buffer(header), ::zmq::send_flags::sndmore);
    s.send(::zmq::buffer(body), blob ? ::zmq::send_flags::sndmore : ::zmq::send_flags::none);
    if (blob) {
        if (owner) {
            ::zmq::message_t m(const_cast<void*>(data), size, free_owner, new BlobOwner(owner));
            s.send(m, ::zmq::send_flags::none);
        } else {
            s.send(::zmq::const_buffer(data, size), ::zmq::send_flags::none);
        }
    }
    return true;
}

}  // namespace

::zmq::context_t& context() {
    static ::zmq::context_t ctx(1);
    return ctx;
}

std::string version() {
    const auto [major, minor, patch] = ::zmq::version();
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

// ---------------------------------------------------------------------------
// Stream / Publisher

Stream::Stream(Publisher& owner, std::string name, std::string type, StreamOptions options)
    : owner_(owner), name_(std::move(name)), type_(std::move(type)), options_(std::move(options)) {}

bool Stream::wanted() {
    if (subscribers_.load(std::memory_order_relaxed) == 0) return false;
    if (options_.max_rate_hz <= 0) return true;
    std::lock_guard lock(rate_mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (now < next_) return false;
    next_ = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                      std::chrono::duration<double>(1.0 / options_.max_rate_hz));
    return true;
}

bool Stream::publish(const google::protobuf::Message& body) { return publish(body, nullptr, nullptr, 0); }

bool Stream::publish(const google::protobuf::Message& body, BlobOwner owner, const void* data, std::size_t size) {
    auto header = proto::make_header(name_, body.GetTypeName());
    std::string bytes;
    body.SerializeToString(&bytes);
    return publish_with_header(std::move(header), std::move(bytes), std::move(owner), data, size);
}

bool Stream::publish_with_header(v1::Header header, std::string body, BlobOwner owner, const void* data,
                                 std::size_t size) {
    header.set_seq(++seq_);
    if (header.topic().empty()) header.set_topic(name_);
    Publisher::Outgoing out;
    out.stream = this;
    header.SerializeToString(&out.header);
    out.body = std::move(body);
    out.owner = std::move(owner);
    out.data = data;
    out.size = size;
    if (out.data && out.size && !out.owner) {
        // No owner: keep a copy so the caller may reuse its buffer.
        auto copy = std::make_shared<std::string>(static_cast<const char*>(data), size);
        out.data = copy->data();
        out.owner = copy;
    }
    return owner_.enqueue(*this, std::move(out));
}

v1::StreamInfo Stream::info() const {
    v1::StreamInfo i;
    i.set_name(name_);
    i.set_endpoint(owner_.endpoint());
    i.set_topic(name_);
    i.set_type(type_);
    i.set_policy(options_.policy);
    i.set_max_rate_hz(options_.max_rate_hz);
    i.set_subscribers(subscribers_.load());
    i.set_sent(sent_.load());
    i.set_dropped(dropped_.load());
    return i;
}

Publisher::Publisher(const std::string& endpoint, SocketOptions options) : options_(options) {
    auto bound = bound_.get_future();
    thread_ = std::thread([this, endpoint] {
        trace::set_thread_name("forge-pub");
        ::zmq::socket_t socket(context(), ::zmq::socket_type::xpub);
        try {
            apply(socket, options_);
            socket.set(::zmq::sockopt::xpub_verbose, 1);
            socket.set(::zmq::sockopt::xpub_verboser, 1);
            socket.bind(endpoint);
            bound_.set_value(resolved_endpoint(socket, endpoint));
        } catch (...) {
            bound_.set_exception(std::current_exception());
            return;
        }
        ::zmq::pollitem_t item{socket.handle(), 0, ZMQ_POLLIN, 0};
        while (running_) {
            // Subscriptions (XPUB): count per prefix.
            ::zmq::message_t msg;
            while (socket.recv(msg, ::zmq::recv_flags::dontwait)) {
                if (msg.size() == 0) continue;
                const auto* p = static_cast<const char*>(msg.data());
                update_subscriptions(std::string_view(p + 1, msg.size() - 1), p[0] == 1);
            }
            std::vector<Outgoing> batch;
            {
                std::unique_lock lock(mutex_);
                if (queue_.empty() && latest_.empty()) {
                    cv_.wait_for(lock, std::chrono::milliseconds(20));
                }
                while (!queue_.empty()) {
                    batch.push_back(std::move(queue_.front()));
                    queue_.pop_front();
                }
                for (auto& [n, o] : latest_) batch.push_back(std::move(o));
                latest_.clear();
            }
            for (auto& o : batch) {
                FORGE_TRACE_SCOPE("zmq.publish", "forge");
                try {
                    if (send_parts(socket, o.stream->name(), o.header, o.body, o.owner, o.data, o.size, true)) {
                        ++o.stream->sent_;
                    } else {
                        ++o.stream->dropped_;
                    }
                } catch (const ::zmq::error_t& e) {
                    ++o.stream->dropped_;
                    log::warn("zmq: publish on {} failed: {}", endpoint_, e.what());
                }
            }
            (void)::zmq::poll(&item, 1, std::chrono::milliseconds(0));
        }
        socket.close();
    });
    endpoint_ = bound.get();  // rethrows bind errors
}

Publisher::~Publisher() { stop(); }

void Publisher::stop() {
    running_ = false;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Publisher::update_subscriptions(std::string_view prefix, bool subscribe) {
    std::lock_guard lock(mutex_);
    auto& n = prefixes_[std::string(prefix)];
    if (subscribe) {
        ++n;
    } else if (n > 0) {
        --n;
    }
    for (auto& [name, s] : streams_) {
        std::size_t count = 0;
        for (const auto& [p, c] : prefixes_) {
            if (name.compare(0, p.size(), p) == 0) count += c;
        }
        s->subscribers_ = count;
    }
}

Stream& Publisher::add(const std::string& name, const std::string& type, StreamOptions options) {
    std::lock_guard lock(mutex_);
    auto it = streams_.find(name);
    if (it != streams_.end()) return *it->second;
    auto s = std::unique_ptr<Stream>(new Stream(*this, name, type, std::move(options)));
    std::size_t count = 0;
    for (const auto& [p, c] : prefixes_) {
        if (name.compare(0, p.size(), p) == 0) count += c;
    }
    s->subscribers_ = count;
    return *streams_.emplace(name, std::move(s)).first->second;
}

Stream* Publisher::find(const std::string& name) {
    std::lock_guard lock(mutex_);
    auto it = streams_.find(name);
    return it == streams_.end() ? nullptr : it->second.get();
}

std::vector<v1::StreamInfo> Publisher::list() const {
    std::lock_guard lock(mutex_);
    std::vector<v1::StreamInfo> out;
    for (const auto& [n, s] : streams_) out.push_back(s->info());
    return out;
}

std::size_t Publisher::queued() const {
    std::lock_guard lock(mutex_);
    return queue_.size() + latest_.size();
}

bool Publisher::enqueue(Stream& s, Outgoing out) {
    if (!running_) return false;
    {
        std::lock_guard lock(mutex_);
        if (s.options_.policy == "latest") {
            if (latest_.count(s.name_)) ++s.dropped_;  // replaced before it was sent
            latest_[s.name_] = std::move(out);
        } else {
            if (queue_.size() >= static_cast<std::size_t>(std::max(options_.send_hwm, 1))) {
                ++s.dropped_;
                return false;
            }
            queue_.push_back(std::move(out));
        }
    }
    cv_.notify_one();
    return true;
}

// ---------------------------------------------------------------------------
// Sender

Sender::Sender(Kind kind, const std::string& endpoint, bool bind, SocketOptions options)
    : kind_(kind),
      socket_(context(), kind == Kind::pub ? ::zmq::socket_type::xpub : ::zmq::socket_type::push) {
    apply(socket_, options);
    if (kind_ == Kind::pub) {
        // Report every (un)subscription so subscriptions() is accurate.
        socket_.set(::zmq::sockopt::xpub_verbose, 1);
        socket_.set(::zmq::sockopt::xpub_verboser, 1);
    }
    bind ? socket_.bind(endpoint) : socket_.connect(endpoint);
}

void Sender::poll_subscriptions() {
    if (kind_ != Kind::pub) return;
    ::zmq::message_t msg;
    while (socket_.recv(msg, ::zmq::recv_flags::dontwait)) {
        if (msg.size() == 0) continue;
        const auto first = static_cast<const unsigned char*>(msg.data())[0];
        if (first == 1) ++subscriptions_;
        if (first == 0 && subscriptions_ > 0) --subscriptions_;
    }
}

std::size_t Sender::subscriptions() {
    poll_subscriptions();
    return static_cast<std::size_t>(subscriptions_);
}

bool Sender::send_frames(std::string_view topic, const std::string& header, const std::string& body,
                         const void* blob, std::size_t size) {
    poll_subscriptions();
    try {
        if (!send_parts(socket_, topic, header, body, nullptr, blob, size, true)) {
            ++dropped_;
            return false;
        }
    } catch (const ::zmq::error_t& e) {
        log::warn("zmq: send on {} failed: {}", endpoint(), e.what());
        ++dropped_;
        return false;
    }
    ++sent_;
    return true;
}

bool Sender::publish(std::string_view topic, const google::protobuf::Message& body, const void* blob,
                     std::size_t size) {
    auto header = proto::make_header(topic, body.GetTypeName());
    auto it = seq_.find(topic);
    if (it == seq_.end()) it = seq_.emplace(std::string(topic), 0).first;
    header.set_seq(++it->second);
    std::string h, b;
    header.SerializeToString(&h);
    body.SerializeToString(&b);
    return send_frames(topic, h, b, blob, size);
}

bool Sender::send(const Message& m) {
    std::string h;
    m.header.SerializeToString(&h);
    return send_frames(m.topic, h, m.body, m.blob.empty() ? nullptr : m.blob.data(), m.blob.size());
}

std::string Sender::endpoint() const { return resolved_endpoint(const_cast<::zmq::socket_t&>(socket_), ""); }

// ---------------------------------------------------------------------------
// Receiver

Receiver::Receiver(Kind kind, const std::string& endpoint, Handler handler)
    : Receiver(kind, endpoint, std::move(handler), Options{}) {}

Receiver::Receiver(Kind kind, const std::string& endpoint, Handler handler, Options options)
    : socket_(context(), kind == Kind::sub ? ::zmq::socket_type::sub : ::zmq::socket_type::pull),
      handler_(std::move(handler)) {
    apply(socket_, options.socket);
    if (kind == Kind::sub) {
        for (const auto& t : options.topics) socket_.set(::zmq::sockopt::subscribe, t);
    }
    options.bind ? socket_.bind(endpoint) : socket_.connect(endpoint);
    endpoint_ = resolved_endpoint(socket_, endpoint);
    thread_ = std::thread([this] { run(); });
}

Receiver::~Receiver() { stop(); }

void Receiver::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void Receiver::run() {
    ::zmq::pollitem_t item{socket_.handle(), 0, ZMQ_POLLIN, 0};
    while (running_) {
        try {
            if (::zmq::poll(&item, 1, kPollInterval) <= 0) continue;
            Message m;
            while (running_ && receive(socket_, m)) {
                ++received_;
                // Gaps in the sender's sequence = messages dropped on the way.
                if (m.header.seq() > 0) {
                    auto& last = last_seq_[m.header.source() + "/" + std::to_string(m.header.pid()) + "/" + m.topic];
                    if (last > 0 && m.header.seq() > last + 1) lost_ += m.header.seq() - last - 1;
                    last = m.header.seq();
                }
                trace::ContextScope scope(m.header.has_trace() ? trace::from_proto(m.header.trace())
                                                               : trace::Context{});
                try {
                    handler_(std::move(m));
                } catch (const std::exception& e) {
                    log::error("zmq: handler for {} failed: {}", endpoint_, e.what());
                }
            }
        } catch (const ::zmq::error_t& e) {
            if (e.num() == ETERM) break;
            log::warn("zmq: receive on {} failed: {}", endpoint_, e.what());
        }
    }
    socket_.close();
}

// ---------------------------------------------------------------------------
// Server

Server::Server(const std::string& endpoint, Handler handler, SocketOptions options)
    : Server(endpoint, std::move(handler), ServerOptions{}, options) {}

Server::Server(const std::string& endpoint, Handler handler, ServerOptions server, SocketOptions options)
    : socket_(context(), ::zmq::socket_type::router),
      handler_(std::move(handler)),
      server_options_(std::move(server)),
      replies_(context(), ::zmq::socket_type::pull) {
    static std::atomic<std::uint64_t> instances{0};
    apply(socket_, options);
    socket_.bind(endpoint);
    endpoint_ = resolved_endpoint(socket_, endpoint);
    replies_endpoint_ = "inproc://forge-server-" + std::to_string(++instances);
    replies_.set(::zmq::sockopt::linger, 0);
    replies_.bind(replies_endpoint_);
    const int n = std::max(server_options_.workers, 1);
    for (int i = 0; i < n; ++i) {
        workers_.emplace_back([this, i, n] {
            trace::set_thread_name(n == 1 ? server_options_.thread_name
                                          : server_options_.thread_name + "-" + std::to_string(i));
            work();
        });
    }
    thread_ = std::thread([this] {
        trace::set_thread_name(server_options_.thread_name + "-io");
        run();
    });
}

Server::~Server() { stop(); }

void Server::stop() {
    // Workers first (they finish the request in hand; queued ones are dropped),
    // then the I/O thread, which still takes their last replies.
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void Server::send_reply(::zmq::socket_t& out, const Job& job, const std::string& reply) {
    for (const auto& frame : job.envelope) out.send(::zmq::buffer(frame), ::zmq::send_flags::sndmore | ::zmq::send_flags::dontwait);
    out.send(::zmq::buffer(reply), ::zmq::send_flags::dontwait);
}

namespace {
std::string busy_reply(const std::string& why) {
    std::string out;
    proto::error_reply(503, why).SerializeToString(&out);
    return out;
}
}  // namespace

void Server::work() {
    ::zmq::socket_t out(context(), ::zmq::socket_type::push);
    out.set(::zmq::sockopt::linger, 0);
    out.connect(replies_endpoint_);
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) break;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        std::string reply;
        if (std::chrono::steady_clock::now() - job.received > server_options_.max_queue_wait) {
            ++rejected_;
            reply = busy_reply("server busy: the request waited too long");
        } else {
            try {
                reply = handler_(job.body);
            } catch (const std::exception& e) {
                proto::error_reply(500, e.what()).SerializeToString(&reply);
            }
        }
        try {
            send_reply(out, job, reply);
        } catch (const ::zmq::error_t& e) {
            if (e.num() == ETERM) break;
            log::warn("zmq: server {} could not reply: {}", endpoint_, e.what());
        }
    }
    out.close();
}

void Server::run() {
    ::zmq::pollitem_t items[] = {{socket_.handle(), 0, ZMQ_POLLIN, 0}, {replies_.handle(), 0, ZMQ_POLLIN, 0}};
    std::vector<::zmq::message_t> frames;
    const auto receive_all = [&](::zmq::socket_t& from) {
        frames.clear();
        for (;;) {
            ::zmq::message_t m;
            if (!from.recv(m, ::zmq::recv_flags::dontwait)) return !frames.empty();
            const bool more = m.more();
            frames.push_back(std::move(m));
            if (!more) return true;
        }
    };
    while (running_) {
        try {
            if (::zmq::poll(items, 2, kPollInterval) <= 0) continue;
            // Replies from the workers: forward them as they are (envelope + body).
            if (items[1].revents & ZMQ_POLLIN) {
                while (receive_all(replies_)) {
                    for (std::size_t i = 0; i < frames.size(); ++i) {
                        socket_.send(frames[i], i + 1 < frames.size() ? ::zmq::send_flags::sndmore
                                                                      : ::zmq::send_flags::dontwait);
                    }
                }
            }
            if (!(items[0].revents & ZMQ_POLLIN)) continue;
            while (receive_all(socket_)) {
                // [identity...][empty][request][more frames ignored]; DEALER clients
                // may leave out the empty delimiter.
                Job job;
                job.received = std::chrono::steady_clock::now();
                std::size_t i = 0;
                bool delimited = false;
                for (; i < frames.size(); ++i) {
                    job.envelope.push_back(frames[i].to_string());
                    if (frames[i].size() == 0) {
                        delimited = true;
                        ++i;
                        break;
                    }
                }
                if (!delimited) {
                    // no delimiter: the first frame is the identity, the next one the request
                    job.envelope.assign(1, frames[0].to_string());
                    i = 1;
                }
                if (i >= frames.size()) continue;  // no request frame
                job.body = frames[i].to_string();
                ++requests_;
                bool queued = false;
                {
                    std::lock_guard lock(mutex_);
                    if (!stopping_ && queue_.size() < server_options_.max_queued) {
                        queue_.push_back(std::move(job));
                        queued = true;
                    }
                }
                if (queued) {
                    cv_.notify_one();
                } else {
                    ++rejected_;
                    send_reply(socket_, job, busy_reply("server busy: too many requests waiting"));
                }
            }
        } catch (const ::zmq::error_t& e) {
            if (e.num() == ETERM) break;
            log::warn("zmq: server {} failed: {}", endpoint_, e.what());
        }
    }
    socket_.close();
    replies_.close();
}

// ---------------------------------------------------------------------------
// Client

Client::Client(std::string endpoint, std::chrono::milliseconds timeout)
    : endpoint_(std::move(endpoint)), timeout_(timeout) {
    char host[128] = {};
    ::gethostname(host, sizeof(host) - 1);
    client_ = proto::source() + "@" + host;
    reconnect();
}

void Client::reconnect() {
    socket_ = std::make_unique<::zmq::socket_t>(context(), ::zmq::socket_type::req);
    socket_->set(::zmq::sockopt::linger, 0);
    socket_->connect(endpoint_);
}

std::string Client::exchange(const std::string& bytes) {
    socket_->send(::zmq::buffer(bytes), ::zmq::send_flags::none);
    ::zmq::pollitem_t item{socket_->handle(), 0, ZMQ_POLLIN, 0};
    if (::zmq::poll(&item, 1, timeout_) <= 0) {
        reconnect();  // a REQ socket is stuck after a lost reply: start over
        throw Error("no reply from " + endpoint_ + " within " + std::to_string(timeout_.count()) + " ms");
    }
    ::zmq::message_t msg;
    (void)socket_->recv(msg);
    return msg.to_string();
}

v1::Reply Client::request(v1::Request req) {
    if (req.client().empty()) req.set_client(client_);
    if (req.token().empty() && !token_.empty()) req.set_token(token_);
    if (!req.has_trace()) {
        const auto ctx = trace::current();
        *req.mutable_trace() = trace::to_proto(ctx.valid() ? trace::child_of(ctx) : trace::new_trace());
    }
    std::string bytes;
    req.SerializeToString(&bytes);
    v1::Reply reply;
    const auto answer = exchange(bytes);
    if (!reply.ParseFromString(answer)) throw Error("invalid reply from " + endpoint_);
    return reply;
}

v1::Reply Client::call(v1::Request req) {
    auto reply = request(std::move(req));
    if (!reply.ok()) throw Error(reply.error(), reply.status());
    return reply;
}

json Client::get(const std::string& id) {
    v1::Request r;
    r.mutable_get()->set_id(id);
    return proto::to_json(call(std::move(r)).value());
}

json Client::set(const std::string& id, const json& value) {
    v1::Request r;
    r.mutable_set()->set_id(id);
    *r.mutable_set()->mutable_value() = proto::to_value(value);
    return proto::to_json(call(std::move(r)).value());
}

void Client::press(const std::string& id) {
    v1::Request r;
    r.mutable_press()->set_id(id);
    call(std::move(r));
}

json Client::call_command(const std::string& id, const json& args) {
    v1::Request r;
    r.mutable_call()->set_id(id);
    *r.mutable_call()->mutable_args() = proto::to_struct(args);
    return proto::to_json(call(std::move(r)).value());
}

v1::Info Client::info() {
    v1::Request r;
    r.mutable_info();
    return call(std::move(r)).info();
}

json Client::request_json(const json& req) {
    const auto answer = exchange(req.dump());
    auto reply = json::parse(answer, nullptr, false);
    if (reply.is_discarded()) throw Error("invalid JSON reply from " + endpoint_);
    return reply;
}

// ---------------------------------------------------------------------------
// serve_control

std::unique_ptr<Server> serve_control(ControlService& service, const std::string& endpoint, int workers) {
    ServerOptions options;
    options.workers = workers;
    options.thread_name = "forge-control";
    return std::make_unique<Server>(endpoint, [&service](std::string_view request) -> std::string {
        Caller transport;
        transport.channel = "zmq";
        // Older JSON commands: a JSON object frame.
        if (!request.empty() && request.front() == '{') {
            auto j = json::parse(request, nullptr, false);
            json reply = j.is_object() ? service.handle_legacy_json(j, transport)
                                       : json{{"ok", false}, {"error", "request must be a JSON object"}, {"status", 400}};
            return reply.dump(-1, ' ', false, json::error_handler_t::replace);
        }
        return service.handle_bytes(request, transport);
    }, options);
}

std::unique_ptr<Server> serve_application(Application& app, const std::string& endpoint) {
    return serve_control(app.control(), endpoint);
}

}  // namespace forge::zmq
