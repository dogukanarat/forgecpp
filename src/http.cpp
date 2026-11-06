#include "forge/http.hpp"

#include "forge/log.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <stdexcept>

namespace forge::http {
namespace {

constexpr const char* kJsonType = "application/json";

void reply_error(Response& res, int status, const std::string& message) {
    reply_json(res, json{{"error", message}, {"status", status}}, status);
}

// Wrap a handler so that exceptions turn into JSON error responses.
template <class Fn>
Handler guarded(Fn fn) {
    return [fn = std::move(fn)](const Request& req, Response& res) {
        try {
            fn(req, res);
        } catch (const HttpError& e) {
            if (!e.body().is_null()) {
                reply_json(res, e.body(), e.status());
            } else {
                reply_error(res, e.status(), e.what());
            }
        } catch (const json::exception& e) {
            reply_error(res, 400, e.what());
        }
    };
}

Handler json_handler(JsonHandler fn) {
    return guarded([fn = std::move(fn)](const Request& req, Response& res) {
        auto out = fn(req);
        if (out.is_null()) {
            res.status = 204;
        } else {
            reply_json(res, out);
        }
    });
}

Handler json_body_handler(JsonBodyHandler fn) {
    return guarded([fn = std::move(fn)](const Request& req, Response& res) {
        json body;
        if (!req.body.empty()) {
            body = json::parse(req.body, nullptr, false);
            if (body.is_discarded()) throw HttpError(400, "request body is not valid JSON");
        }
        auto out = fn(req, body);
        if (out.is_null()) {
            res.status = 204;
        } else {
            reply_json(res, out);
        }
    });
}

json parse_response(const httplib::Result& result, const char* method, const std::string& path) {
    if (!result) {
        throw HttpError(0, std::string(method) + " " + path + " failed: " +
                               httplib::to_string(result.error()));
    }
    json body;
    if (!result->body.empty()) {
        body = json::parse(result->body, nullptr, false);
        if (body.is_discarded()) body = result->body;  // keep non-JSON bodies as a string
    }
    if (result->status >= 400) {
        std::string message = std::string(method) + " " + path + " -> " + std::to_string(result->status);
        if (body.is_object() && body.contains("error") && body["error"].is_string()) {
            message += ": " + body["error"].get<std::string>();
        }
        throw HttpError(result->status, message, body);
    }
    return body;
}

}  // namespace

void reply_json(Response& res, const json& body, int status) {
    res.status = status;
    res.set_content(body.dump(), kJsonType);
}

// ---------------------------------------------------------------------------
// Server

Server::Server() {
    server_.set_exception_handler([](const Request& req, Response& res, std::exception_ptr ep) {
        std::string what = "unknown error";
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
        log::error("http: {} {} failed: {}", req.method, req.path, what);
        reply_error(res, 500, what);
    });
    server_.set_error_handler([](const Request&, Response& res) {
        if (res.body.empty()) reply_error(res, res.status, httplib::status_message(res.status));
    });
    server_.set_logger([](const Request& req, const Response& res) {
        log::debug("http: {} {} -> {}", req.method, req.path, res.status);
    });
    server_.set_pre_routing_handler([this](const Request& req, Response& res) { return pre_route(req, res); });
}

Server::~Server() { stop(); }

Server& Server::get(const std::string& p, Handler h) {
    server_.Get(p, std::move(h));
    return *this;
}
Server& Server::post(const std::string& p, Handler h) {
    server_.Post(p, std::move(h));
    return *this;
}
Server& Server::put(const std::string& p, Handler h) {
    server_.Put(p, std::move(h));
    return *this;
}
Server& Server::patch(const std::string& p, Handler h) {
    server_.Patch(p, std::move(h));
    return *this;
}
Server& Server::del(const std::string& p, Handler h) {
    server_.Delete(p, std::move(h));
    return *this;
}

Server& Server::get_json(const std::string& p, JsonHandler h) { return get(p, json_handler(std::move(h))); }
Server& Server::del_json(const std::string& p, JsonHandler h) { return del(p, json_handler(std::move(h))); }
Server& Server::post_json(const std::string& p, JsonBodyHandler h) {
    return post(p, json_body_handler(std::move(h)));
}
Server& Server::put_json(const std::string& p, JsonBodyHandler h) {
    return put(p, json_body_handler(std::move(h)));
}
Server& Server::patch_json(const std::string& p, JsonBodyHandler h) {
    return patch(p, json_body_handler(std::move(h)));
}

bool Server::mount(const std::string& mount_point, const std::string& dir) {
    return server_.set_mount_point(mount_point, dir);
}

Server& Server::default_header(const std::string& name, const std::string& value) {
    default_headers_.emplace(name, value);
    server_.set_default_headers(default_headers_);
    return *this;
}

int Server::start(const std::string& host, int port) {
    if (running()) throw std::runtime_error("http server already running");
    int bound = port;
    if (port == 0) {
        bound = server_.bind_to_any_port(host);
        if (bound <= 0) throw std::runtime_error("http: cannot bind " + host + " to any port");
    } else if (!server_.bind_to_port(host, port)) {
        throw std::runtime_error("http: cannot bind " + host + ":" + std::to_string(port));
    }
    port_ = bound;
    thread_ = std::thread([this] { server_.listen_after_bind(); });
    server_.wait_until_ready();
    log::info("http: listening on {}:{}", host, bound);
    return bound;
}

void Server::stop() {
    std::vector<std::shared_ptr<EventStream>> streams;
    {
        std::lock_guard lock(mutex_);
        streams = streams_;
    }
    for (auto& s : streams) s->close();  // release the threads serving event clients
    if (thread_.joinable()) {
        server_.stop();
        thread_.join();
        log::debug("http: stopped");
    }
}

bool Server::running() const { return server_.is_running(); }

Server& Server::threads(std::size_t count) {
    count = std::max<std::size_t>(count, 2);
    server_.new_task_queue = [count] { return new httplib::ThreadPool(count, count * 4); };
    return *this;
}

Server& Server::auth(Auth a) {
    std::lock_guard lock(mutex_);
    auth_ = std::move(a);
    return *this;
}

Server& Server::events(const std::string& pattern, std::shared_ptr<EventStream> stream) {
    {
        std::lock_guard lock(mutex_);
        streams_.push_back(stream);
    }
    return get(pattern, stream->handler());
}

Server& Server::assets(const std::vector<web::Asset>& list) {
    std::lock_guard lock(mutex_);
    for (const auto& a : list) {
        if (a.path == nullptr || a.path[0] != '/') continue;
        // FNV-1a over the content: cheap, stable ETag.
        std::uint64_t h = 1469598103934665603ULL;
        for (std::size_t i = 0; i < a.size; ++i) {
            h ^= a.data[i];
            h *= 1099511628211ULL;
        }
        assets_[a.path] = StoredAsset{a, fmt::format("\"{:016x}-{:x}\"", h, a.size), web::mime_type(a.path)};
    }
    return *this;
}

bool Server::has_asset(const std::string& path) const {
    std::lock_guard lock(mutex_);
    return assets_.count(path) > 0;
}

httplib::Server::HandlerResponse Server::pre_route(const Request& req, Response& res) {
    using R = httplib::Server::HandlerResponse;
    if (!authorized(req)) {
        bool basic = false;
        std::string realm;
        {
            std::lock_guard lock(mutex_);
            basic = !auth_.user.empty();
            realm = auth_.realm;
        }
        if (basic) res.set_header("WWW-Authenticate", "Basic realm=\"" + realm + "\", charset=\"UTF-8\"");
        reply_error(res, 401, "authentication required");
        return R::Handled;
    }
    if ((req.method == "GET" || req.method == "HEAD") && serve_asset(req, res)) return R::Handled;
    return R::Unhandled;
}

namespace {
bool constant_time_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}
}  // namespace

bool Server::authorized(const Request& req) const {
    std::lock_guard lock(mutex_);
    if (!auth_.enabled()) return true;
    for (const auto& p : auth_.public_paths) {
        if (req.path == p) return true;
        if (!p.empty() && p.back() == '/' && req.path.compare(0, p.size(), p) == 0) return true;
    }
    const auto header = req.get_header_value("Authorization");
    if (!auth_.token.empty() && constant_time_equal(header, "Bearer " + auth_.token)) return true;
    if (!auth_.user.empty() &&
        constant_time_equal(header, "Basic " + httplib::detail::base64_encode(auth_.user + ":" + auth_.password))) {
        return true;
    }
    return false;
}

bool Server::serve_asset(const Request& req, Response& res) const {
    std::lock_guard lock(mutex_);
    if (assets_.empty()) return false;

    std::string path = req.path;
    if (!path.empty() && path.back() == '/') path += "index.html";
    auto it = assets_.find(path);
    if (it == assets_.end()) {
        // "/dir" -> "/dir/" when "/dir/index.html" exists.
        if (assets_.count(req.path + "/index.html")) {
            res.set_redirect(req.path + "/");
            return true;
        }
        return false;
    }

    const auto& stored = it->second;
    res.set_header("ETag", stored.etag);
    res.set_header("Cache-Control", "no-cache");
    if (req.get_header_value("If-None-Match") == stored.etag) {
        res.status = 304;
        return true;
    }
    res.status = 200;
    res.set_content(reinterpret_cast<const char*>(stored.asset.data), stored.asset.size, stored.mime);
    return true;
}

// ---------------------------------------------------------------------------
// EventStream

namespace {
std::string format_sse(std::string_view event, const json& data) {
    std::string out;
    if (!event.empty()) {
        out += "event: ";
        out += event;
        out += "\n";
    }
    out += "data: ";
    out += data.dump(-1, ' ', false, json::error_handler_t::replace);
    out += "\n\n";
    return out;
}
}  // namespace

struct EventStream::Impl {
    struct Client {
        std::mutex mutex;
        std::condition_variable cv;
        std::string pending;
        bool closed = false;
    };

    static constexpr std::size_t kMaxPending = 1024 * 1024;  // slow client cut-off
    static constexpr auto kHeartbeat = std::chrono::seconds(10);
    static constexpr auto kLivenessCheck = std::chrono::milliseconds(500);

    mutable std::mutex mutex;
    std::vector<std::shared_ptr<Client>> clients;
    std::function<std::vector<Message>()> greeting;
    bool closed = false;

    void remove(const std::shared_ptr<Client>& c) {
        std::lock_guard lock(mutex);
        clients.erase(std::remove(clients.begin(), clients.end(), c), clients.end());
    }
};

EventStream::EventStream(std::size_t max_clients)
    : impl_(std::make_shared<Impl>()), max_clients_(std::max<std::size_t>(max_clients, 1)) {}

EventStream::~EventStream() { close(); }

void EventStream::publish(std::string_view event, const json& data) {
    const auto msg = format_sse(event, data);
    std::vector<std::shared_ptr<Impl::Client>> clients;
    {
        std::lock_guard lock(impl_->mutex);
        clients = impl_->clients;
    }
    for (auto& c : clients) {
        {
            std::lock_guard lock(c->mutex);
            if (c->closed) continue;
            if (c->pending.size() + msg.size() > Impl::kMaxPending) {
                c->closed = true;  // the client cannot keep up; drop it
            } else {
                c->pending += msg;
            }
        }
        c->cv.notify_one();
    }
}

void EventStream::on_connect(std::function<std::vector<Message>()> greeting) {
    std::lock_guard lock(impl_->mutex);
    impl_->greeting = std::move(greeting);
}

std::size_t EventStream::clients() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->clients.size();
}

void EventStream::close() {
    std::vector<std::shared_ptr<Impl::Client>> clients;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->closed = true;
        clients = impl_->clients;
    }
    for (auto& c : clients) {
        {
            std::lock_guard lock(c->mutex);
            c->closed = true;
        }
        c->cv.notify_all();
    }
}

bool EventStream::closed() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->closed;
}

Handler EventStream::handler() {
    return [impl = impl_, max = max_clients_](const Request&, Response& res) {
        auto client = std::make_shared<Impl::Client>();
        std::function<std::vector<Message>()> greeting;
        {
            std::lock_guard lock(impl->mutex);
            if (impl->closed) {
                reply_error(res, 503, "event stream closed");
                return;
            }
            if (impl->clients.size() >= max) {
                reply_error(res, 503, "too many event stream clients");
                return;
            }
            impl->clients.push_back(client);
            greeting = impl->greeting;
        }

        client->pending = "retry: 2000\n\n";
        if (greeting) {
            try {
                for (const auto& m : greeting()) client->pending += format_sse(m.event, m.data);
            } catch (const std::exception& e) {
                log::warn("http: event stream greeting failed: {}", e.what());
            }
        }

        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider(
            "text/event-stream",
            [client](std::size_t, httplib::DataSink& sink) {
                std::string out;
                {
                    std::unique_lock lock(client->mutex);
                    const auto ready = [&] { return client->closed || !client->pending.empty(); };
                    const auto deadline = std::chrono::steady_clock::now() + Impl::kHeartbeat;
                    // Wake up regularly to notice browsers that went away, so
                    // their slot (and HTTP thread) is released quickly.
                    while (!client->cv.wait_for(lock, Impl::kLivenessCheck, ready)) {
                        lock.unlock();
                        const bool alive = sink.is_writable();
                        lock.lock();
                        if (!alive) return false;
                        if (std::chrono::steady_clock::now() >= deadline) break;
                    }
                    if (client->closed) {
                        lock.unlock();
                        sink.done();
                        return true;
                    }
                    out.swap(client->pending);
                }
                if (out.empty()) out = ": keep-alive\n\n";
                return sink.is_writable() && sink.write(out.data(), out.size());
            },
            [impl, client](bool) { impl->remove(client); });
    };
}

// ---------------------------------------------------------------------------
// Client

Client::Client(const std::string& base_url) : client_(base_url) {
    client_.set_connection_timeout(std::chrono::seconds(5));
    client_.set_read_timeout(std::chrono::seconds(10));
    client_.set_write_timeout(std::chrono::seconds(10));
}

Client::~Client() = default;

Client& Client::timeout(std::chrono::milliseconds t) {
    client_.set_connection_timeout(t);
    client_.set_read_timeout(t);
    client_.set_write_timeout(t);
    return *this;
}

Client& Client::bearer_token(const std::string& token) {
    client_.set_bearer_token_auth(token);
    return *this;
}

Client& Client::basic_auth(const std::string& user, const std::string& password) {
    client_.set_basic_auth(user, password);
    return *this;
}

Client& Client::header(const std::string& name, const std::string& value) {
    headers_.emplace(name, value);
    return *this;
}

json Client::get_json(const std::string& path, const Params& params) {
    auto headers = headers_;
    headers.emplace("Accept", kJsonType);
    return parse_response(client_.Get(path, params, headers), "GET", path);
}

json Client::del_json(const std::string& path) {
    return parse_response(client_.Delete(path, headers_), "DELETE", path);
}

json Client::post_json(const std::string& path, const json& body) {
    return parse_response(client_.Post(path, headers_, body.dump(), kJsonType), "POST", path);
}

json Client::put_json(const std::string& path, const json& body) {
    return parse_response(client_.Put(path, headers_, body.dump(), kJsonType), "PUT", path);
}

json Client::patch_json(const std::string& path, const json& body) {
    return parse_response(client_.Patch(path, headers_, body.dump(), kJsonType), "PATCH", path);
}

}  // namespace forge::http
