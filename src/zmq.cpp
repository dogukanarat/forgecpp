#include "forge/zmq.hpp"

#include "forge/log.hpp"
#include "forge/trace.hpp"

#include <zmq_addon.hpp>

#include <cerrno>
#include <chrono>
#include <cstdio>

#include <unistd.h>

namespace forge::zmq {
namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(100);

void apply(::zmq::socket_t& s, const SocketOptions& o) {
    s.set(::zmq::sockopt::sndhwm, o.send_hwm);
    s.set(::zmq::sockopt::rcvhwm, o.recv_hwm);
    s.set(::zmq::sockopt::linger, o.linger_ms);
    s.set(::zmq::sockopt::maxmsgsize, o.max_message_bytes);
}

constexpr std::int64_t kFrameLimit = 8 + 65535;             // a c2 frame
constexpr std::int64_t kBlobLimit = 64LL * 1024 * 1024;     // a stream message
constexpr std::size_t kSequenceTableLimit = 1024;           // sources a Receiver follows

// What is used when the caller left the default (-1).
SocketOptions with_limit(SocketOptions o, std::int64_t limit) {
    if (o.max_message_bytes < 0) o.max_message_bytes = limit;
    return o;
}

std::string resolved_endpoint(::zmq::socket_t& s, const std::string& fallback) {
    try {
        auto e = s.get(::zmq::sockopt::last_endpoint);
        return e.empty() ? fallback : e;
    } catch (const ::zmq::error_t&) {
        return fallback;
    }
}

// Receive one multipart message. Returns false on no message.
bool receive(::zmq::socket_t& s, Message& out) {
    std::vector<::zmq::message_t> parts;
    if (!::zmq::recv_multipart(s, std::back_inserter(parts), ::zmq::recv_flags::dontwait)) return false;
    out = Message{};
    if (!parts.empty()) out.topic = parts[0].to_string();
    if (parts.size() > 1 && parts[1].size() > 0) {
        if (!pb::decode({static_cast<const std::uint8_t*>(parts[1].data()), parts[1].size()}, out.header).ok()) {
            out.header = {};
        }
    }
    if (parts.size() > 2) out.body.assign(static_cast<const char*>(parts[2].data()), parts[2].size());
    if (parts.size() > 3) out.blob.assign(static_cast<const char*>(parts[3].data()), parts[3].size());
    return true;
}

void free_owner(void*, void* hint) { delete static_cast<BlobOwner*>(hint); }

std::string encode_to_string(const pb::Type& type, const void* msg) {
    std::string out(pb::encoded_size(type, msg), '\0');
    std::size_t n = 0;
    if (!pb::encode(type, msg, {reinterpret_cast<std::uint8_t*>(out.data()), out.size()}, n).ok()) {
        throw Error(std::string("cannot encode ") + type.name);
    }
    out.resize(n);
    return out;
}

std::mutex& source_mutex() {
    static std::mutex m;
    return m;
}
std::string& source_name() {
    static std::string name = "app";
    return name;
}

const std::string& host_name() {
    static const std::string host = [] {
        char buf[256] = {};
        ::gethostname(buf, sizeof(buf) - 1);
        return std::string(buf);
    }();
    return host;
}

void put_trace(forge_stream_header& h, const trace::Context& ctx) {
    if (!ctx.valid()) return;
    h.has_trace = true;
    std::copy(ctx.trace_id.begin(), ctx.trace_id.end(), h.trace.trace_id);
    std::copy(ctx.span_id.begin(), ctx.span_id.end(), h.trace.span_id);
}

trace::Context get_trace(const forge_stream_header& h) {
    trace::Context ctx;
    if (!h.has_trace) return ctx;
    std::copy_n(h.trace.trace_id, ctx.trace_id.size(), ctx.trace_id.begin());
    std::copy_n(h.trace.span_id, ctx.span_id.size(), ctx.span_id.begin());
    return ctx;
}

// Source, host, pid, time and the trace context of the calling thread. Text
// longer than a field is cut: a header must always go out.
forge_stream_header make_header(std::string_view topic, const char* type) {
    forge_stream_header h{};
    const auto copy = [](auto& dst, std::string_view src) {
        const auto n = std::min(src.size(), sizeof(dst) - 1);
        std::copy_n(src.data(), n, dst);
        dst[n] = '\0';
    };
    copy(h.source, source());
    copy(h.host, host_name());
    h.pid = static_cast<std::uint32_t>(::getpid());
    copy(h.topic, topic);
    copy(h.type, type);
    h.time_unix_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    put_trace(h, trace::current());
    return h;
}

// Send [topic][header][body][blob?]; the blob without copying when owned.
bool send_parts(::zmq::socket_t& s, std::string_view topic, const std::string& header, const std::string& body,
                const BlobOwner& owner, const void* data, std::size_t size, bool dontwait) {
    const bool blob = data != nullptr && size > 0;
    const auto first =
        dontwait ? (::zmq::send_flags::sndmore | ::zmq::send_flags::dontwait) : ::zmq::send_flags::sndmore;
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

std::vector<std::string> answer(const tmtc::Registry& commands, std::string_view request) {
    std::vector<std::string> frames;
    tmtc::Dispatcher(commands).handle(
        {reinterpret_cast<const std::uint8_t*>(request.data()), request.size()},
        [&](std::span<const std::uint8_t> m) { frames.emplace_back(m.begin(), m.end()); });
    return frames;
}

Message make_message(std::string_view topic, const pb::Type& type, const void* body, std::string blob) {
    Message m;
    m.topic = std::string(topic);
    m.header = make_header(topic, type.name);
    m.body = encode_to_string(type, body);
    m.blob = std::move(blob);
    return m;
}

void set_source(std::string name) {
    std::lock_guard lock(source_mutex());
    source_name() = std::move(name);
}

std::string source() {
    std::lock_guard lock(source_mutex());
    return source_name();
}

json to_json(const StreamInfo& i) {
    return {{"name", i.name},
            {"endpoint", i.endpoint},
            {"topic", i.topic},
            {"type", i.type},
            {"policy", i.policy},
            {"max_rate_hz", i.max_rate_hz},
            {"subscribers", i.subscribers},
            {"sent", i.sent},
            {"dropped", i.dropped}};
}

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

Stream::Stream(Publisher& owner, std::string name, const pb::Type& type, StreamOptions options)
    : owner_(owner), name_(std::move(name)), type_(type), options_(std::move(options)) {}

bool Stream::wanted() {
    if (subscribers_.load(std::memory_order_relaxed) == 0) return watch_due(false);
    if (options_.max_rate_hz <= 0) return true;
    std::lock_guard lock(rate_mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (now < next_) return false;
    next_ = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                      std::chrono::duration<double>(1.0 / options_.max_rate_hz));
    return true;
}

bool Stream::publish(const pb::Type& type, const void* body, BlobOwner owner, const void* data, std::size_t size) {
    if (&type != &type_) {
        throw std::logic_error("stream '" + name_ + "' carries " + type_.name + ", not " + type.name);
    }
    return enqueue(make_header(name_, type_.name), encode_to_string(type, body), std::move(owner), data, size);
}

bool Stream::forward(const Message& m) {
    auto header = m.header;
    if (header.topic[0] == '\0') std::snprintf(header.topic, sizeof(header.topic), "%s", name_.c_str());
    BlobOwner owner;
    const void* data = nullptr;
    if (!m.blob.empty()) {
        auto copy = std::make_shared<std::string>(m.blob);
        data = copy->data();
        owner = copy;
    }
    return enqueue(header, m.body, std::move(owner), data, m.blob.size());
}

Subscription Stream::watch(std::function<void(const Message&)> fn) {
    ++*watching_;
    auto inner = std::make_shared<Subscription>(watchers_.add(std::move(fn)));
    return Subscription([inner, watching = watching_] {
        inner->reset();
        --*watching;
    });
}

// Whether a watcher gets the next message; `take` uses the turn.
bool Stream::watch_due(bool take) {
    if (watching_->load(std::memory_order_relaxed) == 0) return false;
    const std::int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto next = next_watch_ns_.load(std::memory_order_relaxed);
    if (now < next) return false;
    if (!take) return true;
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / watch_rate_hz));
    return next_watch_ns_.compare_exchange_strong(next, now + period.count());
}

bool Stream::enqueue(forge_stream_header header, std::string body, BlobOwner owner, const void* data,
                     std::size_t size) {
    header.seq = ++seq_;
    if (watch_due(true)) {
        Message seen;
        seen.topic = name_;
        seen.header = header;
        seen.body = body;
        watchers_.notify(seen);
    }
    Publisher::Outgoing out;
    out.stream = this;
    out.header = encode_to_string(*pb::type_of<forge_stream_header>, &header);
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

StreamInfo Stream::info() const {
    StreamInfo i;
    i.name = name_;
    i.endpoint = owner_.endpoint();
    i.topic = name_;
    i.type = type_.name;
    i.policy = options_.policy;
    i.max_rate_hz = options_.max_rate_hz;
    i.subscribers = subscribers_.load();
    i.sent = sent_.load();
    i.dropped = dropped_.load();
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
    try {
        endpoint_ = bound.get();  // rethrows bind errors
    } catch (...) {
        thread_.join();  // it has returned: a joinable thread must not be destroyed
        throw;
    }
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

Stream& Publisher::add(const std::string& name, const pb::Type& type, StreamOptions options) {
    std::lock_guard lock(mutex_);
    auto it = streams_.find(name);
    if (it != streams_.end()) {
        if (&it->second->type() != &type) {
            throw std::logic_error("stream '" + name + "' already carries " + it->second->type().name);
        }
        return *it->second;
    }
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

std::vector<StreamInfo> Publisher::list() const {
    std::lock_guard lock(mutex_);
    std::vector<StreamInfo> out;
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
    : kind_(kind), socket_(context(), kind == Kind::pub ? ::zmq::socket_type::xpub : ::zmq::socket_type::push) {
    apply(socket_, with_limit(options, kBlobLimit));
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

bool Sender::send_frames(std::string_view topic, const std::string& header, const std::string& body, const void* blob,
                         std::size_t size) {
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

bool Sender::publish(std::string_view topic, const pb::Type& type, const void* body, const void* blob,
                     std::size_t size) {
    auto header = make_header(topic, type.name);
    auto it = seq_.find(topic);
    if (it == seq_.end()) it = seq_.emplace(std::string(topic), 0).first;
    header.seq = ++it->second;
    return send_frames(topic, encode_to_string(*pb::type_of<forge_stream_header>, &header),
                       encode_to_string(type, body), blob, size);
}

bool Sender::send(const Message& m) {
    return send_frames(m.topic, encode_to_string(*pb::type_of<forge_stream_header>, &m.header), m.body,
                       m.blob.empty() ? nullptr : m.blob.data(), m.blob.size());
}

bool Sender::send_wait(const Message& m, std::chrono::milliseconds timeout) {
    ::zmq::pollitem_t item{socket_.handle(), 0, ZMQ_POLLOUT, 0};
    if (::zmq::poll(&item, 1, timeout) <= 0 || (item.revents & ZMQ_POLLOUT) == 0) return false;
    return send(m);
}

std::string Sender::endpoint() const { return resolved_endpoint(const_cast<::zmq::socket_t&>(socket_), ""); }

// ---------------------------------------------------------------------------
// Receiver

Receiver::Receiver(Kind kind, const std::string& endpoint, Handler handler)
    : Receiver(kind, endpoint, std::move(handler), Options{}) {}

Receiver::Receiver(Kind kind, const std::string& endpoint, Handler handler, const Options& options)
    : socket_(context(), kind == Kind::sub ? ::zmq::socket_type::sub : ::zmq::socket_type::pull),
      handler_(std::move(handler)) {
    apply(socket_, with_limit(options.socket, kBlobLimit));
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
                if (m.header.seq > 0) {
                    if (last_seq_.size() >= kSequenceTableLimit) last_seq_.clear();  // peers come and go
                    auto& last = last_seq_[std::string(pb::view(m.header.source)) + "/" + std::to_string(m.header.pid) +
                                           "/" + m.topic];
                    if (last > 0 && m.header.seq > last + 1) lost_ += m.header.seq - last - 1;
                    last = m.header.seq;
                }
                trace::ContextScope scope(get_trace(m.header));
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
    apply(socket_, with_limit(options, kFrameLimit));
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

void Server::send_reply(::zmq::socket_t& out, const Job& job, const std::vector<std::string>& frames) {
    for (const auto& frame : job.envelope)
        out.send(::zmq::buffer(frame), ::zmq::send_flags::sndmore | ::zmq::send_flags::dontwait);
    if (frames.empty()) {
        out.send(::zmq::const_buffer(nullptr, 0), ::zmq::send_flags::dontwait);  // a REQ client waits for something
        return;
    }
    for (std::size_t i = 0; i < frames.size(); ++i) {
        out.send(::zmq::buffer(frames[i]), i + 1 < frames.size()
                                               ? ::zmq::send_flags::sndmore | ::zmq::send_flags::dontwait
                                               : ::zmq::send_flags::dontwait);
    }
}

std::vector<std::string> Server::busy(std::string_view request) const {
    return server_options_.busy ? server_options_.busy(request) : std::vector<std::string>{};
}

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
        std::vector<std::string> reply;
        if (std::chrono::steady_clock::now() - job.received > server_options_.max_queue_wait) {
            ++rejected_;
            reply = busy(job.body);
        } else {
            try {
                reply = handler_(job.body);
            } catch (const std::exception& e) {
                log::error("zmq: server {}: the handler failed: {}", endpoint_, e.what());
                reply = busy(job.body);
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
                        socket_.send(frames[i],
                                     i + 1 < frames.size() ? ::zmq::send_flags::sndmore : ::zmq::send_flags::dontwait);
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
                std::unique_lock lock(mutex_);
                if (stopping_ || queue_.size() >= server_options_.max_queued) {
                    lock.unlock();
                    ++rejected_;
                    send_reply(socket_, job, busy(job.body));
                    continue;
                }
                queue_.push_back(std::move(job));
                lock.unlock();
                cv_.notify_one();
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
    reconnect();
}

void Client::reconnect() {
    socket_ = std::make_unique<::zmq::socket_t>(context(), ::zmq::socket_type::req);
    socket_->set(::zmq::sockopt::linger, 0);
    socket_->connect(endpoint_);
}

Status Client::request(std::uint16_t id, std::span<const std::uint8_t> payload,
                       const std::function<Status(const c2::Message&)>& on_answer) {
    if (payload.size() > c2::payload_max) return Status::error(module_id::zmq, EMSGSIZE, "request too large");
    const std::uint16_t seq = ++seq_;
    std::vector<std::uint8_t> frame(c2::header_size + payload.size());
    std::size_t n = 0;
    const auto hdr = c2::make_header(c2::flag::req, id, seq, static_cast<std::uint16_t>(payload.size()));
    if (auto s = c2::encode_message(hdr, payload, frame, n); !s.ok()) return s;
    socket_->send(::zmq::buffer(frame.data(), n), ::zmq::send_flags::none);

    ::zmq::pollitem_t item{socket_->handle(), 0, ZMQ_POLLIN, 0};
    if (::zmq::poll(&item, 1, timeout_) <= 0) {
        reconnect();  // a REQ socket is stuck after a lost answer: start over
        return Status::error(module_id::zmq, ETIMEDOUT, "no answer");
    }
    std::vector<::zmq::message_t> frames;
    (void)::zmq::recv_multipart(*socket_, std::back_inserter(frames));

    Status result;
    for (const auto& f : frames) {
        c2::Message m;
        if (!c2::decode_message({static_cast<const std::uint8_t*>(f.data()), f.size()}, m).ok() ||
            m.header.seq != seq || m.header.cmd_id != id) {
            return Status::error(module_id::zmq, EBADMSG, "an answer that is not for this request");
        }
        switch (c2::kind_of(m.header.flags)) {
            case c2::Kind::nack: {
                Status carried;
                if (auto s = c2::nack_status(m.payload, carried); !s.ok()) return s;
                return carried;
            }
            case c2::Kind::ack:
            case c2::Kind::reply:
                if (result.ok()) result = on_answer(m);
                break;
            default: return Status::error(module_id::zmq, EBADMSG, "an answer that is not a reply");
        }
    }
    if (frames.empty()) return Status::error(module_id::zmq, EBADMSG, "an empty answer");
    return result;
}

Status Client::call(std::uint16_t id, const pb::Type& type, const void* req,
                    const std::function<Status(const c2::Message&)>& on_answer) {
    std::vector<std::uint8_t> payload(pb::encoded_size(type, req));
    std::size_t n = 0;
    if (auto s = pb::encode(type, req, payload, n); !s.ok()) return s;
    return request(id, std::span(payload).first(n), on_answer);
}

}  // namespace forge::zmq
