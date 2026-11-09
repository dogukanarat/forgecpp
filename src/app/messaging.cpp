// Messaging on ZeroMQ: [forge].events, [forge].data (streams) and the
// [forge].control command endpoint and the [forge].serial line (c2 frames,
// answered by the application's command registry).
#include "services.hpp"

#include "forge/control.hpp"
#include "forge/metrics.hpp"
#include "forge/tmtc.hpp"

#include <cstring>

namespace forge::detail {
namespace {

// A command the server is too busy to run: a NACK, so the client knows at once.
std::vector<std::string> refuse_busy(std::string_view request) {
    c2::Message m;
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(request.data()), request.size());
    if (!c2::decode_message(bytes, m).ok()) return {};
    const auto nack = c2::nack_payload(Status::error(module_id::tmtc, EAGAIN, "busy"));
    std::string out(c2::header_size + nack.size(), '\0');
    std::size_t n = 0;
    const auto s = c2::encode_message(c2::make_header(c2::flag::nack, m.header.cmd_id, m.header.seq, nack.size()), nack,
                                      {reinterpret_cast<std::uint8_t*>(out.data()), out.size()}, n);
    if (!s.ok()) return {};
    return {out};
}

}  // namespace

template <pb::Message T>
void Messaging::publish_event(const char* topic, const T& body) {
    if (!events_pub_) return;
    if (auto* s = events_pub_->find(topic); s && s->wanted()) s->publish(body);
}

void Messaging::start(const Config& cfg) {
    auto logger = app_.logger();
    const auto events = cfg.value<std::string>("forge.events");
    if (!events.empty()) {
        zmq::SocketOptions so;
        so.send_hwm = 1000;
        events_pub_ = std::make_unique<zmq::Publisher>(events, so);
        endpoints_.set("events", events_pub_->endpoint());
        events_pub_->add<forge_state_report>("state");
        events_pub_->add<forge_alarm>("alarms");
        events_pub_->add<forge_log_record>("log");
        events_pub_->add<forge_audit_record>("audit");
        events_pub_->add<forge_health_reply>("health");
        events_pub_->add<forge_metrics_reply>("metrics");
        subscriptions_.push_back(app_.alarms().on_change([this] {
            auto* s = events_pub_->find("alarms");
            if (!s || s->subscribers() == 0) return;
            // One message per alarm: a subscriber keeps the latest of each id.
            for (const auto& a : app_.alarms().list()) {
                forge_alarm msg{};
                fill(msg, a);
                s->publish(msg);
            }
        }));
        subscriptions_.push_back(app_.audit().on_record([this](const Audit::Record& r) {
            forge_audit_record msg{};
            fill(msg, r);
            publish_event("audit", msg);
        }));
        subscriptions_.push_back(app_.health().on_change([this](const Health::Report& h) {
            forge_health_reply msg{};
            fill(msg, h);
            publish_event("health", msg);
        }));
        subscriptions_.push_back(log::add_listener([this](const log::Record& r) {
            forge_log_record msg{};
            fill(msg, r);
            publish_event("log", msg);
        }));
        const auto interval = std::chrono::seconds(std::max(cfg.value<int>("forge.metrics_interval_s"), 1));
        app_.every(interval, [this] {
            auto* s = events_pub_->find("metrics");
            if (!s || !s->wanted()) return;
            // The same pages as the "metrics" command, one message each.
            const auto* cmd = app_.commands().find(forge_msg_id_metrics);
            std::uint32_t more = 0;
            std::array<std::uint8_t, forge_metrics_reply_size> buf{};
            do {
                std::size_t n = 0;
                if (!cmd->run_binary({}, buf, n, more).ok()) return;
                forge_metrics_reply page{};
                if (pb::decode(std::span(buf).first(n), page).ok()) s->publish(page);
            } while (more != 0);
        });
        logger->info("forge: events on {}", events_pub_->endpoint());
    }
    const auto data = cfg.value<std::string>("forge.data");
    if (!data.empty()) {
        std::lock_guard lock(data_mutex_);
        data_pub_ = std::make_unique<zmq::Publisher>(data);
        endpoints_.set("data", data_pub_->endpoint());
        logger->info("forge: streams on {}", data_pub_->endpoint());
    }
}

void Messaging::start_control(const Config& cfg) {
    auto logger = app_.logger();
    const auto control = cfg.value<std::string>("forge.control");
    if (!control.empty()) {
        zmq::ServerOptions options;
        options.workers = cfg.value<int>("forge.control_workers");
        options.thread_name = "forge-control";
        options.busy = refuse_busy;
        control_server_ = std::make_unique<zmq::Server>(control, [this](std::string_view request) {
            CallerScope caller(app_.control().identify("link", "zmq", {}));
            return zmq::answer(app_.commands(), request);
        }, options);
        endpoints_.set("control", control_server_->endpoint());
        logger->info("forge: commands on {}", control_server_->endpoint());
        // The link's counters on /metrics (the server outlives the registry's reads: reset() clears them).
        auto& m = app_.metrics();
        const metrics::Registry::Labels link{{"link", "zmq"}};
        m.gauge_fn("forge.link.requests", link, [this] {
            return control_server_ ? double(control_server_->requests()) : 0;
        }, "Commands received on the link");
        m.gauge_fn("forge.link.refused", link, [this] {
            return control_server_ ? double(control_server_->rejected()) : 0;
        }, "Commands refused because the server was busy");
    }
    const auto device = cfg.value<std::string>("serial.device");
    if (!device.empty()) {
        serial_options_.device = device;
        endpoints_.set("serial", device);
        serial_options_.baud = cfg.value<int>("serial.baud");
        serial_options_.framing.frame_size = static_cast<std::size_t>(cfg.value<int>("serial.frame_size"));
        serial_options_.thread_name = "forge-serial";
        serial_options_.on_request = [this](std::span<const std::uint8_t> frame, const auto& send) {
            CallerScope caller(app_.control().identify("link", "serial", {}));
            tmtc::Dispatcher(app_.commands()).handle(frame, send);
        };
        // A device that is not there yet (USB still enumerating) is not fatal:
        // health says so and the line is tried again every second.
        open_serial();
        app_.every(std::chrono::seconds(1), [this] { check_serial(); });
        auto& m = app_.metrics();
        const metrics::Registry::Labels link{{"link", "serial"}, {"device", device}};
        const auto stat = [this](auto member) {
            return [this, member] {
                std::lock_guard lock(serial_stats_mutex_);
                return static_cast<double>(serial_stats_.*member);
            };
        };
        const auto framing = [this](auto member) {
            return [this, member] {
                std::lock_guard lock(serial_stats_mutex_);
                return static_cast<double>(serial_stats_.framing.*member);
            };
        };
        m.gauge_fn("forge.link.requests", link, stat(&serial::Link::Stats::requests_in),
                   "Commands received on the link");
        m.gauge_fn("forge.link.refused", link, stat(&serial::Link::Stats::requests_refused),
                   "Commands refused because the link was busy");
        m.gauge_fn("forge.serial.messages_received", link, framing(&serial::Framing::Stats::messages_received),
                   "Messages assembled from the line");
        m.gauge_fn("forge.serial.frames_dropped", link, framing(&serial::Framing::Stats::frames_dropped),
                   "Fragments with a bad CRC or COBS");
        m.gauge_fn("forge.serial.messages_dropped", link, framing(&serial::Framing::Stats::messages_dropped),
                   "Messages lost to a missing or out-of-order fragment");
    }
}

bool Messaging::open_serial() {
    auto logger = app_.logger();
    try {
        serial_ = std::make_unique<serial::Link>(serial_options_);
    } catch (const std::exception& e) {
        if (serial_error_ != e.what()) {  // the same reason again: only in debug
            logger->warn("forge: cannot open {}: {} (trying again every second)", serial_options_.device, e.what());
            serial_error_ = e.what();
        } else {
            logger->debug("forge: cannot open {}: {}", serial_options_.device, e.what());
        }
        app_.health().set("serial", HealthState::failed, e.what(), false);
        return false;
    }
    serial_error_.clear();
    logger->info("forge: commands on {} at {} baud", serial_options_.device, serial_options_.baud);
    app_.health().set("serial", HealthState::ok, "commands on " + serial_options_.device, false);
    return true;
}

void Messaging::check_serial() {
    if (serial_stopped_) return;
    if (serial_) {
        std::lock_guard lock(serial_stats_mutex_);
        serial_stats_ = serial_->stats();
    }
    if (serial_ && !serial_->connected()) {
        app_.logger()->warn("forge: {} went away; waiting for it", serial_options_.device);
        app_.health().set("serial", HealthState::failed, "line gone: " + serial_options_.device, false);
        serial_.reset();
    }
    if (!serial_) open_serial();
}

void Messaging::stop_control() {
    serial_stopped_ = true;
    if (control_server_) control_server_->stop();
    serial_.reset();
}

void Messaging::stop() {
    subscriptions_.clear();
    {
        std::lock_guard lock(data_mutex_);
        if (data_pub_) data_pub_->stop();
    }
    if (events_pub_) events_pub_->stop();
}

void Messaging::reset() {
    serial_stopped_ = true;
    auto& m = app_.metrics();
    for (const auto& name : {"forge.link.requests", "forge.link.refused"}) {
        m.remove(name, {{"link", "zmq"}});
        if (!serial_options_.device.empty()) m.remove(name, {{"link", "serial"}, {"device", serial_options_.device}});
    }
    if (!serial_options_.device.empty()) {
        for (const auto& name :
             {"forge.serial.messages_received", "forge.serial.frames_dropped", "forge.serial.messages_dropped"}) {
            m.remove(name, {{"link", "serial"}, {"device", serial_options_.device}});
        }
    }
    for (const auto& info : stream_list()) {
        for (const auto& name : {"forge.stream.sent", "forge.stream.dropped", "forge.stream.subscribers"}) {
            m.remove(name, {{"topic", info.name}});
        }
    }
    serial_.reset();
    control_server_.reset();
    {
        std::lock_guard lock(data_mutex_);
        data_pub_.reset();
    }
    events_pub_.reset();
}

zmq::Stream& Messaging::stream(const std::string& name, const pb::Type& type, const zmq::StreamOptions& o) {
    std::lock_guard lock(data_mutex_);
    if (!data_pub_) {
        // No [forge].data configured: an ephemeral port, listed in the endpoints.
        const auto ep = app_.config().value<std::string>("forge.data");
        data_pub_ = std::make_unique<zmq::Publisher>(ep.empty() ? "tcp://*:0" : ep);
        endpoints_.set("data", data_pub_->endpoint());
        app_.logger()->info("forge: streams on {}", data_pub_->endpoint());
    }
    const bool known = data_pub_->find(name) != nullptr;
    auto& s = data_pub_->add(name, type, o);
    if (!known) {
        // Its counters on /metrics, by topic. The stream lives as long as the
        // publisher, which reset() destroys after the registry stops being read.
        auto& m = app_.metrics();
        const metrics::Registry::Labels topic{{"topic", name}};
        m.gauge_fn("forge.stream.sent", topic, [&s] { return static_cast<double>(s.info().sent); },
                   "Messages sent on the stream");
        m.gauge_fn("forge.stream.dropped", topic, [&s] { return static_cast<double>(s.info().dropped); },
                   "Messages dropped because the queue was full or the policy kept only the newest");
        m.gauge_fn("forge.stream.subscribers", topic, [&s] { return static_cast<double>(s.subscribers()); },
                   "Subscribers of the stream");
    }
    return s;
}

zmq::Stream* Messaging::find_stream(const std::string& name) {
    std::lock_guard lock(data_mutex_);
    return data_pub_ ? data_pub_->find(name) : nullptr;
}

std::vector<zmq::StreamInfo> Messaging::stream_list() const {
    std::lock_guard lock(data_mutex_);
    return data_pub_ ? data_pub_->list() : std::vector<zmq::StreamInfo>{};
}

bool Messaging::wants_state() const {
    if (!events_pub_) return false;
    auto* s = events_pub_->find("state");
    return s && s->subscribers() > 0;
}

void Messaging::publish_state(const json& current) {
    auto* s = events_pub_ ? events_pub_->find("state") : nullptr;
    if (!s) return;
    for (const auto& [id, value] : current.items()) {
        auto it = last_state_.find(id);
        if (it != last_state_.end() && *it == value) continue;
        forge_state_report msg{};
        pb::assign_truncated(msg.id, id);
        pb::assign_truncated(msg.value, value.dump(-1, ' ', false, json::error_handler_t::replace));
        s->publish(msg);
    }
    last_state_ = current;
}

}  // namespace forge::detail
