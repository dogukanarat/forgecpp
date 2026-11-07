#include "forge/zmq.hpp"

#include "forge/application.hpp"
#include "forge/http.hpp"
#include "forge/log.hpp"
#include "forge/version.hpp"
#include "forge/web.hpp"

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

// Receive one multipart message. Returns false on timeout / no message.
bool receive(::zmq::socket_t& s, Message& out) {
    std::vector<::zmq::message_t> parts;
    if (!::zmq::recv_multipart(s, std::back_inserter(parts), ::zmq::recv_flags::dontwait)) return false;
    out = Message{};
    if (!parts.empty()) out.topic = parts[0].to_string();
    if (parts.size() > 1 && parts[1].size() > 0) {
        out.header = json::parse(parts[1].to_string_view(), nullptr, false);
        if (out.header.is_discarded()) out.header = json::object();
    }
    if (parts.size() > 2) out.payload.assign(static_cast<const char*>(parts[2].data()), parts[2].size());
    return true;
}

json error_reply(const std::string& message, int status) {
    return {{"ok", false}, {"error", message}, {"status", status}};
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

bool Sender::send(std::string_view topic, const json& header, const void* data, std::size_t size) {
    poll_subscriptions();
    const auto header_text = header.dump();
    const bool has_payload = data != nullptr && size > 0;
    try {
        // The first frame decides: if it is accepted, the rest follow atomically.
        if (!socket_.send(::zmq::buffer(topic.data(), topic.size()),
                          ::zmq::send_flags::sndmore | ::zmq::send_flags::dontwait)) {
            ++dropped_;
            return false;
        }
        socket_.send(::zmq::buffer(header_text),
                     has_payload ? ::zmq::send_flags::sndmore : ::zmq::send_flags::none);
        if (has_payload) socket_.send(::zmq::const_buffer(data, size), ::zmq::send_flags::none);
    } catch (const ::zmq::error_t& e) {
        log::warn("zmq: send on {} failed: {}", endpoint(), e.what());
        ++dropped_;
        return false;
    }
    ++sent_;
    return true;
}

bool Sender::send(const Message& m) {
    return send(m.topic, m.header, m.payload.empty() ? nullptr : m.payload.data(), m.payload.size());
}

std::string Sender::endpoint() const {
    return resolved_endpoint(const_cast<::zmq::socket_t&>(socket_), "");
}

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
    : socket_(context(), ::zmq::socket_type::rep), handler_(std::move(handler)) {
    apply(socket_, options);
    socket_.bind(endpoint);
    endpoint_ = resolved_endpoint(socket_, endpoint);
    thread_ = std::thread([this] { run(); });
}

Server::~Server() { stop(); }

void Server::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void Server::run() {
    ::zmq::pollitem_t item{socket_.handle(), 0, ZMQ_POLLIN, 0};
    while (running_) {
        try {
            if (::zmq::poll(&item, 1, kPollInterval) <= 0) continue;
            ::zmq::message_t msg;
            if (!socket_.recv(msg, ::zmq::recv_flags::dontwait)) continue;
            // REP sockets may receive multipart requests; only the first frame matters.
            while (msg.more()) {
                ::zmq::message_t rest;
                (void)socket_.recv(rest);
                if (!rest.more()) break;
            }
            ++requests_;

            json reply;
            const auto request = json::parse(msg.to_string_view(), nullptr, false);
            if (request.is_discarded() || !request.is_object()) {
                reply = error_reply("request must be a JSON object", 400);
            } else {
                try {
                    reply = handler_(request);
                    if (!reply.is_object()) reply = json{{"result", reply}};
                    if (!reply.contains("ok")) reply["ok"] = true;
                } catch (const http::HttpError& e) {
                    reply = error_reply(e.what(), e.status());
                } catch (const Error& e) {
                    reply = error_reply(e.what(), e.status() ? e.status() : 400);
                } catch (const json::exception& e) {
                    reply = error_reply(e.what(), 400);
                } catch (const std::exception& e) {
                    reply = error_reply(e.what(), 500);
                }
            }
            socket_.send(::zmq::buffer(reply.dump(-1, ' ', false, json::error_handler_t::replace)),
                         ::zmq::send_flags::none);
        } catch (const ::zmq::error_t& e) {
            if (e.num() == ETERM) break;
            log::warn("zmq: server {} failed: {}", endpoint_, e.what());
        }
    }
    socket_.close();
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

json Client::request(const json& req) {
    socket_->send(::zmq::buffer(req.dump()), ::zmq::send_flags::none);
    ::zmq::pollitem_t item{socket_->handle(), 0, ZMQ_POLLIN, 0};
    if (::zmq::poll(&item, 1, timeout_) <= 0) {
        reconnect();  // a REQ socket is stuck after a lost reply: start over
        throw Error("no reply from " + endpoint_ + " within " + std::to_string(timeout_.count()) + " ms");
    }
    ::zmq::message_t msg;
    (void)socket_->recv(msg);
    auto reply = json::parse(msg.to_string_view(), nullptr, false);
    if (reply.is_discarded()) throw Error("invalid JSON reply from " + endpoint_);
    return reply;
}

json Client::call(const std::string& cmd, const json& args) {
    json req = args.is_object() ? args : json::object();
    req["cmd"] = cmd;
    auto reply = request(req);
    if (!reply.value("ok", false)) {
        throw Error(reply.value("error", std::string("request failed")), reply.value("status", 500));
    }
    return reply;
}

// ---------------------------------------------------------------------------
// serve_application

std::unique_ptr<Server> serve_application(Application& app, const std::string& endpoint) {
    auto handler = [&app](const json& req) -> json {
        const auto cmd = req.value("cmd", std::string());
        auto id = [&]() -> std::string {
            if (!req.contains("id") || !req["id"].is_string()) throw Error("\"id\" is required", 400);
            return req["id"].get<std::string>();
        };
        auto dashboard = [&app]() -> web::Dashboard& {
            if (!app.dashboard()) throw Error("application has no controls", 404);
            return *app.dashboard();
        };

        if (cmd == "ping") return {{"pong", true}};
        if (cmd == "info") {
            return {{"name", app.info().name},
                    {"version", app.info().version},
                    {"description", app.info().description},
                    {"forgecpp", version_string},
                    {"pid", ::getpid()},
                    {"uptime_s", std::chrono::duration_cast<std::chrono::seconds>(app.uptime()).count()},
                    {"alarms", app.alarms().summary()}};
        }
        if (cmd == "ui") return app.invoke([&] { return json{{"ui", dashboard().schema()}}; });
        if (cmd == "state") return app.invoke([&] { return json{{"state", dashboard().state()}}; });
        if (cmd == "get") {
            const auto key = id();
            return app.invoke([&] { return json{{"id", key}, {"value", dashboard().get(key)}}; });
        }
        if (cmd == "set") {
            const auto key = id();
            if (!req.contains("value")) throw Error("\"value\" is required", 400);
            return app.invoke([&] {
                dashboard().set(key, req["value"]);
                app.logger()->info("zmq: set '{}' = {}", key, req["value"].dump());
                return json{{"id", key}, {"value", dashboard().get(key)}};
            });
        }
        if (cmd == "press") {
            const auto key = id();
            return app.invoke([&] {
                dashboard().press(key);
                app.logger()->info("zmq: pressed '{}'", key);
                return json{{"id", key}};
            });
        }
        if (cmd == "alarms") return {{"alarms", app.alarms().to_json()}};
        if (cmd == "ack") {
            const auto key = id();
            if (key == "all") return {{"acknowledged", app.alarms().acknowledge_all("zmq")}};
            if (!app.alarms().acknowledge(key, "zmq")) throw Error("unknown alarm '" + key + "'", 404);
            return {{"acknowledged", 1}};
        }
        throw Error("unknown command '" + cmd + "' (ping|info|ui|state|get|set|press|alarms|ack)", 400);
    };
    auto server = std::make_unique<Server>(endpoint, handler);
    log::info("zmq: control server on {}", server->endpoint());
    return server;
}

}  // namespace forge::zmq
