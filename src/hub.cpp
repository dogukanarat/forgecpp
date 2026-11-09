#include "forge/hub.hpp"

#include "forge/log.hpp"
#include "forge/zmq.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>

namespace forge::hub {

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

std::string instance_of(const std::string& unit) {
    const auto at = unit.find('@');
    const auto dot = unit.rfind('.');
    if (at == std::string::npos || dot == std::string::npos || dot <= at + 1) return {};
    return unit.substr(at + 1, dot - at - 1);
}

Status register_app(const std::string& endpoint, const forge_hub_app_registration& app, forge_hub_register_reply& reply,
                    std::chrono::milliseconds timeout) {
    forge_hub_register_request req{};
    req.has_app = true;
    req.app = app;
    return zmq::Client(endpoint, timeout).call(forge_hub_msg_id_register, req, reply);
}

Status unregister_app(const std::string& endpoint, const forge_hub_unregister_request& request,
                      std::chrono::milliseconds timeout) {
    forge_hub_unregister_reply reply{};
    return zmq::Client(endpoint, timeout).call(forge_hub_msg_id_unregister, request, reply);
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
        const auto app = build_();
        forge_hub_register_reply reply{};
        const auto status = register_app(endpoint_, app, reply, timeout_);
        const bool ok = status.ok();
        if (ok && !registered_) {
            log::info("hub: registered with {} at {}", pb::view(reply.hub), endpoint_);
        } else if (!ok && registered_) {
            log::info("hub: {} does not answer any more{}", endpoint_,
                      retry_.count() > 0 ? "; will register again when it is back" : "");
        } else if (!ok && status.errno_code() != ETIMEDOUT) {
            log::warn("hub: registration refused: {}", status.to_string());
        }
        registered_ = ok;
        if (ok) ever_registered_ = true;
        lock.lock();
        if (ok) last_ = app;
        if (retry_.count() <= 0) break;
        cv_.wait_for(lock, retry_, [this] { return stopping_; });
    }
}

void Client::stop(forge_hub_unregister_reason reason, int exit_code) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_ && !thread_.joinable()) return;
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (!ever_registered_) return;
    forge_hub_unregister_request req{};
    std::memcpy(req.name, last_.name, sizeof(req.name));
    req.pid = last_.pid;
    req.reason = reason;
    req.exit_code = exit_code;
    if (unregister_app(endpoint_, req, timeout_).ok()) {
        log::debug("hub: unregistered ({})",
                   reason == forge_hub_unregister_reason_REASON_SIGNAL ? "stop request" : "exit");
    }
    registered_ = false;
}

}  // namespace forge::hub
