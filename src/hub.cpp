#include "forge/hub.hpp"

#include "forge/log.hpp"
#include "forge/zmq.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace forge::hub {

v1::RestartPolicy::Mode parse_mode(const std::string& text) {
    std::string t = text;
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return std::tolower(c); });
    std::replace(t.begin(), t.end(), '_', '-');
    if (t == "never" || t == "no") return v1::RestartPolicy::MODE_NEVER;
    if (t == "on-failure") return v1::RestartPolicy::MODE_ON_FAILURE;
    if (t == "always") return v1::RestartPolicy::MODE_ALWAYS;
    throw ConfigError("hub.restart must be never, on-failure or always (not '" + text + "')");
}

std::string mode_name(v1::RestartPolicy::Mode mode) {
    switch (mode) {
        case v1::RestartPolicy::MODE_ON_FAILURE: return "on-failure";
        case v1::RestartPolicy::MODE_ALWAYS: return "always";
        default: return "never";
    }
}

v1::RestartPolicy policy_from_config(const Config& cfg) {
    v1::RestartPolicy p;
    p.set_mode(parse_mode(cfg.get_or<std::string>("hub.restart", "never")));
    p.set_max_restarts(static_cast<std::uint32_t>(std::max(cfg.get_or<int>("hub.max_restarts", 3), 0)));
    p.set_window_s(static_cast<std::uint32_t>(std::max(cfg.get_or<int>("hub.restart_window_s", 600), 1)));
    p.set_delay_s(static_cast<std::uint32_t>(std::max(cfg.get_or<int>("hub.restart_delay_s", 2), 0)));
    p.set_critical(cfg.get_or<bool>("hub.critical", false));
    p.set_hang_timeout_s(static_cast<std::uint32_t>(std::max(cfg.get_or<int>("hub.hang_timeout_s", 0), 0)));
    return p;
}

std::string unit_of(int pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/cgroup");
    std::string line, unit;
    while (std::getline(in, line)) {
        // cgroup v2: "0::/system.slice/radio-controller.service"; v1: "1:name=systemd:/system.slice/x.service"
        const auto path = line.substr(line.rfind(':') + 1);
        std::size_t start = 0;
        while (start < path.size()) {
            auto end = path.find('/', start);
            if (end == std::string::npos) end = path.size();
            const auto part = path.substr(start, end - start);
            if (part.size() > 8 && part.compare(part.size() - 8, 8, ".service") == 0) unit = part;
            start = end + 1;
        }
        if (!unit.empty()) break;
    }
    return unit;
}

std::optional<v1::HubReply> exchange(const std::string& endpoint, const v1::HubRequest& request,
                                     std::chrono::milliseconds timeout) {
    try {
        ::zmq::socket_t s(zmq::context(), ::zmq::socket_type::req);
        s.set(::zmq::sockopt::linger, 0);
        s.set(::zmq::sockopt::sndtimeo, static_cast<int>(timeout.count()));
        s.set(::zmq::sockopt::rcvtimeo, static_cast<int>(timeout.count()));
        s.connect(endpoint);
        const auto bytes = request.SerializeAsString();
        if (!s.send(::zmq::buffer(bytes), ::zmq::send_flags::none)) return std::nullopt;
        ::zmq::message_t reply;
        if (!s.recv(reply, ::zmq::recv_flags::none)) return std::nullopt;
        v1::HubReply r;
        if (!r.ParseFromArray(reply.data(), static_cast<int>(reply.size()))) return std::nullopt;
        return r;
    } catch (const std::exception& e) {
        log::debug("hub: {}: {}", endpoint, e.what());
        return std::nullopt;
    }
}

Client::Client(std::string endpoint, std::chrono::seconds retry, Builder build, std::chrono::milliseconds timeout)
    : endpoint_(std::move(endpoint)), retry_(retry), build_(std::move(build)), timeout_(timeout) {}

Client::~Client() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Client::start() {
    if (thread_.joinable()) return;
    thread_ = std::thread([this] { run(); });
}

void Client::run() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        lock.unlock();
        v1::HubRequest req;
        *req.mutable_register_() = build_();
        const auto reply = exchange(endpoint_, req, timeout_);
        const bool ok = reply && reply->ok();
        if (ok && !registered_) {
            log::info("hub: registered with {} at {}", reply->hub(), endpoint_);
        } else if (!ok && registered_) {
            log::info("hub: {} does not answer any more{}", endpoint_,
                      retry_.count() > 0 ? "; will register again when it is back" : "");
        } else if (reply && !reply->ok()) {
            log::warn("hub: registration refused: {}", reply->error());
        }
        registered_ = ok;
        if (ok) ever_registered_ = true;
        lock.lock();
        if (ok) last_ = req.register_();
        if (retry_.count() <= 0) break;
        cv_.wait_for(lock, retry_, [this] { return stopping_; });
    }
}

void Client::stop(v1::Unregister::Reason reason, int exit_code) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_ && !thread_.joinable()) return;
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (!ever_registered_) return;
    v1::HubRequest req;
    auto* u = req.mutable_unregister();
    u->set_name(last_.name());
    u->set_pid(last_.pid());
    u->set_reason(reason);
    u->set_exit_code(exit_code);
    if (exchange(endpoint_, req, timeout_)) {
        log::debug("hub: unregistered ({})", reason == v1::Unregister::REASON_SIGNAL ? "stop request" : "exit");
    }
    registered_ = false;
}

}  // namespace forge::hub
