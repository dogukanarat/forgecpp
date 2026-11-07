#pragma once
// HTTP server and client built on cpp-httplib, speaking JSON via nlohmann/json.
//
//   forge::http::Server server;
//   server.get_json("/api/status", [](const forge::http::Request&) {
//       return forge::json{{"ok", true}};
//   });
//   server.post_json("/api/items/:id", [](const auto& req, const forge::json& body) {
//       auto id = req.path_params.at("id");
//       if (!body.contains("name")) throw forge::http::HttpError(400, "name is required");
//       return forge::json{{"id", id}};
//   });
//   server.start("0.0.0.0", 8080);            // runs in a background thread
//
//   forge::http::Client client("http://127.0.0.1:8080");
//   forge::json status = client.get_json("/api/status");

#include "forge/assets.hpp"

#include "forge/error.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace forge {
using json = nlohmann::json;
}

namespace forge::http {

using Request = httplib::Request;
using Response = httplib::Response;
using Headers = httplib::Headers;
using Params = httplib::Params;

/// Throw from a handler to answer with `status` and {"error": message} (or
/// `body`). Thrown by Client for transport errors (status 0) and HTTP status
/// >= 400. Handlers may throw any forge::Error the same way.
class HttpError : public forge::Error {
public:
    HttpError(int status, const std::string& message, json body = nullptr)
        : forge::Error(status, message, std::move(body)) {}
    const json& body() const { return details(); }
};

using Handler = httplib::Server::Handler;
using JsonHandler = std::function<json(const Request&)>;
using JsonBodyHandler = std::function<json(const Request&, const json& body)>;

/// Build a JSON response.
void reply_json(Response& res, const json& body, int status = 200);

/// Server-Sent Events broadcaster: every connected browser receives what
/// you publish(). Thread-safe; register with Server::events().
///
///   auto stream = std::make_shared<forge::http::EventStream>();
///   server.events("/events", stream);
///   stream->publish("temperature", {{"value", 21.5}});
///   // browser: new EventSource("/events").addEventListener("temperature", ...)
class EventStream {
public:
    struct Message {
        std::string event;
        json data;
    };

    explicit EventStream(std::size_t max_clients = 16);
    ~EventStream();

    EventStream(const EventStream&) = delete;
    EventStream& operator=(const EventStream&) = delete;

    /// Send an event to every connected client.
    void publish(std::string_view event, const json& data);

    /// Messages sent to each client right after it connects (e.g. a snapshot).
    /// Called on the HTTP thread that serves the new client.
    void on_connect(std::function<std::vector<Message>()> greeting);

    std::size_t clients() const;
    std::size_t max_clients() const { return max_clients_; }

    /// Disconnect everybody and refuse new clients.
    void close();
    bool closed() const;

    /// httplib handler serving the stream (used by Server::events()).
    Handler handler();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    std::size_t max_clients_;
};

/// HTTP authentication for a Server. Requests must present either
/// "Authorization: Basic <user:password>" or "Authorization: Bearer <token>".
struct Auth {
    std::string user;
    std::string password;
    std::string token;
    std::string realm = "forgecpp";
    /// Paths reachable without credentials (exact match, or prefix ending in '/').
    std::vector<std::string> public_paths{"/health"};
    /// Custom check of the Authorization header (e.g. forge::Auth users);
    /// replaces user/password/token when set.
    std::function<bool(const std::string& authorization)> check;

    bool enabled() const { return !user.empty() || !token.empty() || static_cast<bool>(check); }
};

class Server {
public:
    Server();
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Raw httplib handlers.
    Server& get(const std::string& pattern, Handler handler);
    Server& post(const std::string& pattern, Handler handler);
    Server& put(const std::string& pattern, Handler handler);
    Server& patch(const std::string& pattern, Handler handler);
    Server& del(const std::string& pattern, Handler handler);

    // JSON handlers: the returned json is sent with status 200
    // (or 204 when it is null). Request bodies are parsed for you; invalid
    // JSON is answered with 400. Throw HttpError for other statuses.
    Server& get_json(const std::string& pattern, JsonHandler handler);
    Server& del_json(const std::string& pattern, JsonHandler handler);
    Server& post_json(const std::string& pattern, JsonBodyHandler handler);
    Server& put_json(const std::string& pattern, JsonBodyHandler handler);
    Server& patch_json(const std::string& pattern, JsonBodyHandler handler);

    /// Serve files from `dir` under `mount_point` (e.g. "/", "./www").
    bool mount(const std::string& mount_point, const std::string& dir);

    /// Add a header to every response (e.g. CORS).
    Server& default_header(const std::string& name, const std::string& value);

    /// Serve in-memory assets (see forge_add_web()). "dir/" serves "dir/index.html".
    /// Assets are matched before routes and mounts; later calls override paths.
    Server& assets(const std::vector<web::Asset>& assets);
    bool has_asset(const std::string& path) const;

    /// Server-Sent Events endpoint; the stream is closed when the server stops.
    Server& events(const std::string& pattern, std::shared_ptr<EventStream> stream);

    /// Require credentials for every request except Auth::public_paths.
    Server& auth(Auth auth);

    /// Worker threads (each open EventStream client keeps one busy). Call before start().
    Server& threads(std::size_t count);

    /// Bind and serve in a background thread. `port` 0 picks a free port.
    /// Returns the bound port; throws std::runtime_error if binding fails.
    int start(const std::string& host, int port);
    void stop();
    bool running() const;
    int port() const { return port_; }

    httplib::Server& native() { return server_; }

private:
    struct StoredAsset {
        web::Asset asset;
        std::string etag;
        std::string mime;
    };

    httplib::Server::HandlerResponse pre_route(const Request& req, Response& res);
    bool authorized(const Request& req) const;
    bool serve_asset(const Request& req, Response& res) const;

    httplib::Server server_;
    Headers default_headers_;
    std::thread thread_;
    std::atomic<int> port_{0};

    mutable std::mutex mutex_;
    std::map<std::string, StoredAsset, std::less<>> assets_;
    std::vector<std::shared_ptr<EventStream>> streams_;
    Auth auth_;
};

class Client {
public:
    /// `base_url` like "http://host:8080" (https:// requires OpenSSL support).
    explicit Client(const std::string& base_url);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    Client& timeout(std::chrono::milliseconds t);
    Client& bearer_token(const std::string& token);
    Client& basic_auth(const std::string& user, const std::string& password);
    Client& header(const std::string& name, const std::string& value);

    json get_json(const std::string& path, const Params& params = {});
    json del_json(const std::string& path);
    json post_json(const std::string& path, const json& body);
    json put_json(const std::string& path, const json& body);
    json patch_json(const std::string& path, const json& body);

    httplib::Client& native() { return client_; }

private:
    httplib::Client client_;
    Headers headers_;
};

}  // namespace forge::http
