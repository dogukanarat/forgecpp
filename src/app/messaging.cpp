// Messaging: forge.v1 on ZeroMQ - [forge].events, [forge].data (streams) and
// the [forge].control endpoint.
#include "services.hpp"

#include "forge/control.hpp"
#include "forge/proto.hpp"

namespace forge::detail {

void Messaging::start(const Config& cfg) {
    auto logger = app_.logger();
    const auto events = cfg.value<std::string>("forge.events");
    if (!events.empty()) {
        zmq::SocketOptions so;
        so.send_hwm = 1000;
        events_pub_ = std::make_unique<zmq::Publisher>(events, so);
        events_pub_->add("state", "forge.v1.Values");
        events_pub_->add("alarms", "forge.v1.AlarmList");
        events_pub_->add("log", "forge.v1.LogRecord");
        events_pub_->add("audit", "forge.v1.AuditRecord");
        events_pub_->add("health", "forge.v1.Health");
        events_pub_->add("metrics", "forge.v1.MetricsSnapshot");
        app_.alarms().on_change([this] {
            if (!events_pub_) return;
            auto* s = events_pub_->find("alarms");
            if (!s || s->subscribers() == 0) return;
            v1::Request r;
            r.mutable_alarms();
            Caller internal;
            const auto reply = app_.control().handle(r, internal);
            s->publish(reply.alarms());
        });
        app_.audit().on_record([this](const v1::AuditRecord& r) { publish_event("audit", r); });
        app_.health().on_change([this](const v1::Health& h) { publish_event("health", h); });
        log_listener_ = log::add_listener([this](const v1::LogRecord& r) { publish_event("log", r); });
        const auto interval = std::chrono::seconds(std::max(cfg.value<int>("forge.metrics_interval_s"), 1));
        app_.every(interval, [this] { publish_event("metrics", metrics::registry().snapshot()); });
        logger->info("forge: events on {}", events_pub_->endpoint());
    }
    const auto data = cfg.value<std::string>("forge.data");
    if (!data.empty()) {
        std::lock_guard lock(data_mutex_);
        data_pub_ = std::make_unique<zmq::Publisher>(data);
        logger->info("forge: streams on {}", data_pub_->endpoint());
    }
    const auto control = cfg.value<std::string>("forge.control");
    if (!control.empty()) {
        control_server_ = zmq::serve_control(app_.control(), control, cfg.value<int>("forge.control_workers"));
        logger->info("forge: control on {}", control_server_->endpoint());
    }
}

void Messaging::stop_control() {
    if (control_server_) control_server_->stop();
}

void Messaging::stop() {
    if (log_listener_ >= 0) log::remove_listener(log_listener_);
    log_listener_ = -1;
    {
        std::lock_guard lock(data_mutex_);
        if (data_pub_) data_pub_->stop();
    }
    if (events_pub_) events_pub_->stop();
}

void Messaging::reset() {
    control_server_.reset();
    {
        std::lock_guard lock(data_mutex_);
        data_pub_.reset();
    }
    events_pub_.reset();
}

zmq::Stream& Messaging::stream(const std::string& name, const std::string& type, const zmq::StreamOptions& o) {
    std::lock_guard lock(data_mutex_);
    if (!data_pub_) {
        // No [forge].data configured: an ephemeral port, listed in Info.endpoints.
        const auto ep = app_.config().value<std::string>("forge.data");
        data_pub_ = std::make_unique<zmq::Publisher>(ep.empty() ? "tcp://*:0" : ep);
        app_.logger()->info("forge: streams on {}", data_pub_->endpoint());
    }
    return data_pub_->add(name, type, o);
}

v1::StreamList Messaging::stream_list() const {
    v1::StreamList l;
    std::lock_guard lock(data_mutex_);
    if (data_pub_) {
        for (auto& s : data_pub_->list()) *l.add_streams() = std::move(s);
    }
    return l;
}

void Messaging::add_endpoints(std::map<std::string, std::string>& e) const {
    if (control_server_) e["control"] = control_server_->endpoint();
    if (events_pub_) e["events"] = events_pub_->endpoint();
    std::lock_guard lock(data_mutex_);
    if (data_pub_) e["data"] = data_pub_->endpoint();
}

void Messaging::publish_event(const std::string& topic, const google::protobuf::Message& body) {
    if (!events_pub_) return;
    if (auto* s = events_pub_->find(topic); s && s->wanted()) s->publish(body);
}

bool Messaging::wants_state() const {
    if (!events_pub_) return false;
    auto* s = events_pub_->find("state");
    return s && s->subscribers() > 0;
}

void Messaging::publish_state(const json& current) {
    auto* s = events_pub_ ? events_pub_->find("state") : nullptr;
    if (!s) return;
    v1::Values v;
    for (const auto& [id, value] : current.items()) {
        auto it = last_state_.find(id);
        if (it == last_state_.end() || *it != value) (*v.mutable_values())[id] = proto::to_value(value);
    }
    last_state_ = current;
    if (v.values_size() > 0) s->publish(v);
}

}  // namespace forge::detail
