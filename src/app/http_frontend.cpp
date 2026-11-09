// HttpFrontend: the HTTP server with the framework's routes, the built-in
// pages, live events and logs for the browser, and the card layouts.
#include "services.hpp"

#include "forge/component.hpp"
#include "forge/control.hpp"
#include "forge/trace.hpp"
#include "forge/version.hpp"
#include "forge/web.hpp"

#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>

namespace forge::detail {

// Keeps the latest log lines for the dashboard and streams new ones to it
// (a log listener: called on the logging background thread).
class WebLog {
public:
    WebLog(std::shared_ptr<http::EventStream> stream, std::size_t history, log::Level min)
        : stream_(std::move(stream)), max_(std::max<std::size_t>(history, 1)), min_(min) {}
    void start() {
        subscription_ = log::add_listener([this](const log::Record& r) { on_record(r); });
    }

    json history() {
        std::lock_guard lock(mutex_);
        return json(std::vector<json>(history_.begin(), history_.end()));
    }

private:
    void on_record(const log::Record& r) {
        if (r.level < min_) return;
        const auto t = static_cast<std::time_t>(r.time_unix_ns / 1000000000);
        const auto ms = (r.time_unix_ns / 1000000) % 1000;
        std::tm tm{};
        ::localtime_r(&t, &tm);
        json entry{{"time", std::format("{:02}:{:02}:{:02}.{:03}", tm.tm_hour, tm.tm_min, tm.tm_sec, ms)},
                   {"level", log::to_string(r.level)},
                   {"logger", r.logger},
                   {"message", r.message}};
        {
            std::lock_guard lock(mutex_);
            history_.push_back(entry);
            while (history_.size() > max_) history_.pop_front();
        }
        stream_->publish("log", entry);
    }

    std::shared_ptr<http::EventStream> stream_;
    std::size_t max_;
    log::Level min_;
    std::mutex mutex_;
    std::deque<json> history_;
    Subscription subscription_;  // last: it goes first, before what on_record uses
};

void HttpFrontend::create(const Config& cfg) {
    if (!cfg.value<bool>("http.enabled")) return;
    auto& auth = app_.auth();
    auto logger = app_.logger();
    http_ = std::make_unique<http::Server>();
    http_->threads(static_cast<std::size_t>(std::max(cfg.value<int>("http.threads"), 2)));

    // Credentials: [auth] users (roles are checked per request by ControlService).
    // Anonymous callers get [auth].default_role, unless [auth].anonymous = false.
    if (auth.enabled() && !cfg.value<bool>("auth.anonymous")) {
        http::Auth a;
        a.check = [&auth](const std::string& header) { return auth.by_http_header(header).has_value(); };
        a.public_paths.push_back("/metrics");
        http_->auth(a);
    } else if (!auth.enabled() && !is_loopback(cfg.value<std::string>("http.host"))) {
        logger->warn("http: no [auth] configured - anyone who can reach port {} can use the API",
                     cfg.value<int>("http.port"));
    }

    http_->get("/health", [this](const http::Request& req, http::Response& res) {
        auto& health = app_.health();
        const auto h = health.snapshot();
        json components = json::array();
        for (const auto& c : h.components) {
            components.push_back(
                {{"name", c.name}, {"state", to_string(c.state)}, {"message", c.message}, {"critical", c.critical}});
        }
        const auto overall = health.overall();
        const auto loop_age = app_.loop_age_ms();
        // The 1 s heartbeat runs on the main loop: an old one means the loop is stuck.
        const char* status = app_.stopping() ? "stopping" : (loop_age > 5000 ? "stalled" : "ok");
        json j{{"status", status},
               {"pid", ::getpid()},
               {"loop_age_ms", loop_age},
               {"health", to_string(overall)},
               {"components", std::move(components)},
               {"name", app_.info().name},
               {"version", app_.info().version},
               {"uptime_s", std::chrono::duration_cast<std::chrono::seconds>(app_.uptime()).count()},
               {"alarms", app_.alarms().summary()}};
        // ?strict=1: 503 while a critical component has failed (load balancers, probes).
        res.status = (req.has_param("strict") && overall == HealthState::failed) ? 503 : 200;
        res.set_content(j.dump(), "application/json");
    });
    // Prometheus scrape endpoint.
    http_->get("/metrics", [](const http::Request&, http::Response& res) {
        res.set_content(metrics::registry().prometheus(), "text/plain; version=0.0.4");
    });
    app_.setup_http(*http_);
    for (auto& c : app_.components_) c->setup_http(*http_);
    for (auto& h : app_.hooks_.http) h(app_, *http_);
    create_web(cfg);
}

void HttpFrontend::create_web(const Config& cfg) {
    if (!cfg.value<bool>("web.enabled")) return;
    auto logger = app_.logger();
    auto* dashboard = app_.dashboard();
    auto& control = app_.control();
    auto& alarms = app_.alarms();

    events_ = std::make_shared<http::EventStream>(cfg.value<std::size_t>("web.max_clients"));
    events_->on_connect([this, dashboard, &alarms] {
        std::vector<http::EventStream::Message> greeting;
        if (!app_.stopping()) {
            try {
                greeting.push_back(
                    {"state", app_.invoke_for(app_.request_timeout(), [dashboard] { return dashboard->state(); })});
            } catch (const std::exception& e) {
                // main loop busy: the next state event brings the values
                log::debug("web: greeting without the state: {}", e.what());
            }
        }
        greeting.push_back({"alarms", alarms.to_json()});
        return greeting;
    });

    // Pages: the built-in dashboard under /forge/, the application's own pages
    // (embedded with forge_add_web, or served from [web].root while developing).
    http_->assets(web::builtin_assets());
    const auto root = cfg.value<std::string>("web.root");
    if (!root.empty()) {
        if (!http_->mount("/", root)) throw ConfigError("web.root '" + root + "' is not a directory");
        logger->info("web: serving pages from {}", root);
        if (cfg.value<bool>("web.live_reload")) pages_root_ = root;
    } else {
        http_->assets(web::registered_assets());
    }

    if (cfg.value<bool>("web.logs")) {
        weblog_ = std::make_shared<WebLog>(events_, cfg.value<std::size_t>("web.log_history"),
                                           log::parse_level(cfg.value<std::string>("web.log_level")));
        weblog_->start();
    }

    const std::string api = "/forge/api";
    http_->get_json(api + "/info", [this](const http::Request&) {
        const auto& info = app_.info();
        return json{{"name", info.name},
                    {"instance", app_.instance()},
                    {"version", info.version},
                    {"description", info.description},
                    {"forgecpp", version_string},
                    {"hostname", host_name()},
                    {"pid", ::getpid()},
                    {"uptime_s", std::chrono::duration_cast<std::chrono::seconds>(app_.uptime()).count()},
                    {"logs", weblog_ != nullptr},
                    {"protocol", "c2"},
                    {"endpoints", app_.endpoints()}};
    });
    // What needs the main loop waits at most [forge].request_timeout_ms for it.
    http_->get_json(api + "/ui", [this, dashboard](const http::Request&) {
        return app_.invoke_for(app_.request_timeout(), [dashboard] { return dashboard->schema(); });
    });
    http_->get_json(api + "/state", [this, dashboard](const http::Request&) {
        return app_.invoke_for(app_.request_timeout(), [dashboard] { return dashboard->state(); });
    });
    http_->get_json(api + "/controls/:id", [this, dashboard](const http::Request& req) {
        const auto id = req.path_params.at("id");
        return json{
            {"id", id},
            {"value", app_.invoke_for(app_.request_timeout(), [dashboard, &id] { return dashboard->get(id); })}};
    });
    auto web_caller = [&control](const http::Request& req) {
        return control.identify("web", req.remote_addr, {}, req.get_header_value("Authorization"));
    };
    http_->post_json(api + "/controls/:id",
                     [this, dashboard, &control, web_caller](const http::Request& req, const json& body) {
        const auto id = req.path_params.at("id");
        const auto caller = web_caller(req);
        const bool button =
            app_.invoke_for(app_.request_timeout(), [&] { return dashboard->control(id).kind == web::Kind::button; });
        if (button) {
            control.press(id, caller);
        } else {
            if (!(body.is_object() && body.contains("value"))) {
                throw Error(400, "request body must be {\"value\": ...}");
            }
            control.set(id, body["value"], caller);
        }
        return json{{"id", id}, {"value", control.get(id, caller)}};
    });
    http_->post_json(api + "/commands/:id", [&control, web_caller](const http::Request& req, const json& body) {
        const auto id = req.path_params.at("id");
        json args = body.is_object() && body.contains("args") ? body["args"] : body;
        if (args.is_null()) args = json::object();
        return json{{"id", id}, {"result", control.call(id, args, web_caller(req))}};
    });
    http_->post_json(api + "/leases/:id", [&control, web_caller](const http::Request& req, const json& body) {
        const auto id = req.path_params.at("id");
        const auto s = control.lease(id, body.is_object() ? body.value("seconds", 60u) : 60u, web_caller(req));
        return json{{"id", id}, {"seconds", s.count()}};
    });
    http_->get_json(api + "/leases", [&control](const http::Request&) { return control.leases(); });
    http_->del_json(api + "/leases/:id", [&control, web_caller](const http::Request& req) {
        control.release(req.path_params.at("id"), web_caller(req));
        return json(nullptr);
    });
    http_->get_json(api + "/whoami", [web_caller](const http::Request& req) {
        const auto c = web_caller(req);
        return json{{"user", c.user}, {"role", to_string(c.role)}, {"client", c.client}};
    });

    // Card layouts of the pages (forge.layout in forge.js): kept on the device, so
    // every browser shows the same arrangement. A signed-in user ([auth]) saves
    // their own; ?scope=device (admins) changes the device's, which everybody
    // without their own sees.
    layouts_.load(cfg.value<std::string>("web.layout_file"), app_.id(), logger);
    const auto page_of = [](const http::Request& req) {
        auto page = req.path_params.at("page");
        if (!LayoutStore::valid_page(page)) throw Error(400, "invalid page name");
        return page;
    };
    // Whose layout a change is: the signed-in user's, or the device's.
    const auto owner_of = [this, &control](const http::Request& req) {
        const auto caller = control.identify("web", req.remote_addr, {}, req.get_header_value("Authorization"));
        if (req.get_param_value("scope") == "device" || caller.user.empty()) {
            if (app_.auth().enabled() && caller.role < Role::operator_) {
                throw Error(403, "changing the device's layout needs the operator role");
            }
            return std::string();
        }
        return caller.user;
    };
    http_->get_json(api + "/layout/:page", [this, page_of, &control](const http::Request& req) {
        const auto caller = control.identify("web", req.remote_addr, {}, req.get_header_value("Authorization"));
        return layouts_.get(page_of(req), req.get_param_value("scope") == "device" ? std::string() : caller.user);
    });
    http_->put_json(api + "/layout/:page", [this, page_of, owner_of](const http::Request& req, const json& body) {
        const auto page = page_of(req);
        if (!body.is_object() || !body.contains("items") || !body["items"].is_array() || body["items"].size() > 500 ||
            req.body.size() > 256 * 1024) {
            throw Error(400, "expected {\"items\": [...]} (at most 500 items)");
        }
        const auto owner = owner_of(req);
        const bool saved = layouts_.put(page, body, owner);
        return json{{"page", page}, {"saved", saved}, {"scope", owner.empty() ? "device" : "user"}};
    });
    http_->del_json(api + "/layout/:page", [this, page_of, owner_of](const http::Request& req) {
        layouts_.remove(page_of(req), owner_of(req));
        return json(nullptr);
    });

    // Read-only views that do not need the main loop.
    http_->get_json(api + "/metrics", [](const http::Request& req) {
        return metrics::to_json(metrics::registry().snapshot(req.get_param_value("prefix")));
    });
    http_->get_json(api + "/audit", [this](const http::Request& req) {
        json a = json::array();
        const auto n = req.has_param("limit") ? std::stoul(req.get_param_value("limit")) : 200;
        for (const auto& r : app_.audit().list(n)) a.push_back(to_json(r));
        return a;
    });
    http_->get_json(api + "/health", [this](const http::Request&) { return to_json(app_.health().snapshot()); });
    http_->get_json(api + "/streams", [this](const http::Request&) {
        json a = json::array();
        for (const auto& i : app_.stream_list()) a.push_back(zmq::to_json(i));
        return json{{"streams", std::move(a)}};
    });
    http_->get(api + "/trace", [](const http::Request& req, http::Response& res) {
        const unsigned seconds = req.has_param("seconds") ? std::stoul(req.get_param_value("seconds")) : 0;
        auto d = trace::dump(seconds);
        res.set_header("Content-Disposition", "attachment; filename=\"trace.json\"");
        res.set_content(std::move(d.chrome_json), "application/json");
    });
    // The commands of the program, the same handlers as over a link, in JSON.
    http_->get_json(api + "/tmtc", [this](const http::Request&) {
        json list = json::array();
        for (const auto* c : app_.commands().list()) {
            list.push_back({{"id", c->id},
                            {"name", c->name},
                            {"request", c->req ? json(c->req->name) : json(nullptr)},
                            {"reply", c->rsp->name},
                            {"push", c->is_push()}});
        }
        return list;
    });
    // A command's messages, for the panel's forms.
    http_->get_json(api + "/tmtc/:name", [this](const http::Request& req) {
        const auto& name = req.path_params.at("name");
        const auto* cmd = app_.commands().find(std::string_view(name));
        if (!cmd) throw Error(404, "no command '" + name + "'");
        return json{{"id", cmd->id},
                    {"name", cmd->name},
                    {"push", cmd->is_push()},
                    {"request", cmd->req ? pb::schema(*cmd->req) : json(nullptr)},
                    {"reply", pb::schema(*cmd->rsp)}};
    });
    // The messages of a stream as they go out, a few per second, in JSON.
    http_->get(api + "/streams/:name/watch", [this](const http::Request& req, http::Response& res) {
        watch_stream(req.path_params.at("name"))->handler()(req, res);
    });
    http_->post_json(api + "/tmtc/:name", [this, &control](const http::Request& req, const json& body) {
        const auto& name = req.path_params.at("name");
        const bool number = !name.empty() && name.find_first_not_of("0123456789") == std::string::npos;
        const auto* cmd = number && name.size() <= 5
                              ? app_.commands().find(static_cast<std::uint16_t>(std::stoul(name)))
                              : app_.commands().find(std::string_view(name));
        if (!cmd) throw Error(404, "no command '" + name + "'");
        if (cmd->is_push()) throw Error(501, "'" + name + "' is a report, not a command");
        CallerScope caller(control.identify("web", req.remote_addr, {}, req.get_header_value("Authorization")));
        return cmd->run_json(body);
    });
    http_->get_json(api + "/logs",
                    [this](const http::Request&) { return weblog_ ? weblog_->history() : json::array(); });
    http_->get_json(api + "/alarms", [&alarms](const http::Request&) { return alarms.to_json(); });
    http_->post_json(api + "/alarms/ack", [&alarms](const http::Request& req, const json&) {
        return json{{"acknowledged", alarms.acknowledge_all("web " + req.remote_addr)}};
    });
    http_->post_json(api + "/alarms/:id/ack", [&alarms](const http::Request& req, const json&) {
        const auto id = req.path_params.at("id");
        if (!alarms.acknowledge(id, "web " + req.remote_addr)) throw Error(404, "unknown alarm '" + id + "'");
        return to_json(*alarms.get(id));
    });
    subscriptions_.push_back(alarms.on_change([this] { alarms_dirty_ = true; }));
    subscriptions_.push_back(app_.audit().on_record([this](const Audit::Record& r) {
        if (events_) events_->publish("audit", to_json(r));
    }));
    http_->events(api + "/events", events_);

    // "/" shows the application's own index.html when there is one, else the dashboard.
    http_->get("/", [](const http::Request&, http::Response& res) {
        res.set_redirect("forge/");
    });  // relative: also behind a proxy
}

void HttpFrontend::start(const Config& cfg) {
    if (!http_) return;
    const auto host = cfg.value<std::string>("http.host");
    http_->start(host, cfg.value<int>("http.port"));
    if (events_) {
        const auto refresh = std::max(cfg.value<int>("web.refresh_ms"), 50);
        app_.every(std::chrono::milliseconds(refresh), [this] { push_state(); });
        app_.logger()->info("web dashboard: http://{}:{}/forge/", is_loopback(host) ? "127.0.0.1" : host_name(),
                            http_->port());
    }
    app_.every(std::chrono::seconds(5), [this] { drop_unwatched_streams(); });
    if (events_ && !pages_root_.empty()) {
        // Developing pages: a saved file reloads the open pages (not /forge/).
        pages_signature_ = pages_signature(pages_root_);
        app_.every(std::chrono::milliseconds(500), [this] {
            const auto now = pages_signature(pages_root_);
            if (now == pages_signature_) return;
            pages_signature_ = now;
            events_->publish("reload", json::object());
        });
        app_.logger()->info("web: pages reload when a file under {} changes", pages_root_);
    }
}

std::size_t HttpFrontend::pages_signature(const std::string& root) {
    // Names, sizes and times of the files: cheap enough for a page directory.
    std::size_t h = 0;
    std::error_code ec;
    std::size_t n = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator() && n < 5000; it.increment(ec), ++n) {
        if (!it->is_regular_file(ec)) continue;
        const auto t = it->last_write_time(ec).time_since_epoch().count();
        const auto size = it->file_size(ec);
        h = h * 1000003U ^ std::hash<std::string>{}(it->path().string()) ^ static_cast<std::size_t>(t) ^ size;
    }
    return h;
}

void HttpFrontend::stop() {
    if (http_) http_->stop();
    subscriptions_.clear();
    weblog_.reset();
    std::lock_guard lock(watch_mutex_);
    watches_.clear();
}

std::shared_ptr<http::EventStream> HttpFrontend::watch_stream(const std::string& name) {
    std::lock_guard lock(watch_mutex_);
    if (auto it = watches_.find(name); it != watches_.end()) return it->second.events;
    auto* stream = app_.messaging_->find_stream(name);
    if (!stream) throw Error(404, "no stream '" + name + "'");
    auto events = std::make_shared<http::EventStream>(4);
    const pb::Type& type = stream->type();
    auto subscription = stream->watch([weak = std::weak_ptr(events), &type](const zmq::Message& m) {
        auto events = weak.lock();
        if (!events || events->clients() == 0) return;
        std::vector<char> body(type.size);
        const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(m.body.data()), m.body.size());
        if (!pb::decode(type, bytes, body.data()).ok()) return;
        events->publish("message", json{{"seq", m.header.seq},
                                        {"time_unix_ns", m.header.time_unix_ns},
                                        {"source", pb::view(m.header.source)},
                                        {"type", type.name},
                                        {"body", pb::to_json(type, body.data())}});
    });
    watches_[name] = {events, std::move(subscription), std::chrono::steady_clock::now()};
    return events;
}

// A stream nobody has watched for a while costs nothing again.
void HttpFrontend::drop_unwatched_streams() {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(watch_mutex_);
    std::erase_if(watches_, [&](const auto& w) {
        return w.second.events->clients() == 0 && now - w.second.since > std::chrono::seconds(10);
    });
}

void HttpFrontend::reset() {
    http_.reset();
    events_.reset();
}

void HttpFrontend::publish(const std::string& event, const json& data) {
    if (events_) events_->publish("app", json{{"event", event}, {"data", data}});
}

void HttpFrontend::push_state() {
    auto* dashboard = app_.dashboard();
    if (!dashboard || !events_ || events_->clients() == 0) return;
    if (alarms_dirty_.exchange(false)) events_->publish("alarms", app_.alarms().to_json());
    auto changed = dashboard->changes();
    if (!changed.empty()) events_->publish("state", changed);
}

void HttpFrontend::add_endpoints(std::map<std::string, std::string>& e) const {
    if (http_ && http_->port() > 0) e["http"] = std::format("http://{}:{}/", host_name(), http_->port());
}

// ---------------------------------------------------------------------------
// LayoutStore

std::string LayoutStore::default_file(const std::string& name) {
    // systemd's StateDirectory= (the units forge_add_app generates have one).
    if (const char* sd = std::getenv("STATE_DIRECTORY"); sd && *sd) {
        std::string dir(sd);
        return dir.substr(0, dir.find(':')) + "/web-layout.json";
    }
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg)
        return std::string(xdg) + "/" + name + "/web-layout.json";
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.local/state/" + name + "/web-layout.json";
    }
    return {};
}

bool LayoutStore::valid_page(const std::string& p) {
    return !p.empty() && p.size() <= 64 && std::all_of(p.begin(), p.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.';
    });
}

void LayoutStore::load(std::string file, const std::string& app_name, const Logger& logger) {
    std::lock_guard lock(mutex_);
    logger_ = logger;
    if (file.empty()) file = default_file(app_name);
    if (file == "-") file.clear();
    file_ = std::move(file);
    if (file_.empty() || !std::filesystem::exists(file_)) return;
    try {
        auto j = load_json(file_);
        if (j.is_object()) layouts_ = std::move(j);
    } catch (const std::exception& e) {
        logger_->warn("ignoring {}: {}", file_, e.what());
    }
}

// File: {"<page>": layout, ..., "@users": {"<user>": {"<page>": layout}}}
// ("@" cannot start a page name).
namespace {
constexpr const char* kUsers = "@users";
}  // namespace

json LayoutStore::get(const std::string& page, const std::string& user) const {
    std::lock_guard lock(mutex_);
    if (!user.empty() && layouts_.contains(kUsers)) {
        const auto& users = layouts_[kUsers];
        if (users.contains(user) && users[user].contains(page)) {
            json l = users[user][page];
            if (l.is_object()) l["scope"] = "user";
            return l;
        }
    }
    if (!layouts_.contains(page)) return nullptr;
    json l = layouts_[page];
    if (l.is_object()) l["scope"] = "device";
    return l;
}

bool LayoutStore::put(const std::string& page, json layout, const std::string& user) {
    std::lock_guard lock(mutex_);
    if (layout.is_object()) layout.erase("scope");
    if (user.empty()) {
        layouts_[page] = std::move(layout);
    } else {
        layouts_[kUsers][user][page] = std::move(layout);
    }
    save();
    return !file_.empty();
}

void LayoutStore::remove(const std::string& page, const std::string& user) {
    std::lock_guard lock(mutex_);
    if (user.empty()) {
        layouts_.erase(page);
    } else if (layouts_.contains(kUsers) && layouts_[kUsers].contains(user)) {
        auto& mine = layouts_[kUsers][user];
        mine.erase(page);
        if (mine.empty()) layouts_[kUsers].erase(user);
        if (layouts_[kUsers].empty()) layouts_.erase(kUsers);
    }
    save();
}

void LayoutStore::save() {
    if (file_.empty()) return;
    try {
        const std::filesystem::path p(file_);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
        save_json(p, layouts_, 1);
    } catch (const std::exception& e) {
        if (logger_) logger_->warn("cannot save {}: {}", file_, e.what());
    }
}

}  // namespace forge::detail
