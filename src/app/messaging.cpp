// Messaging on ZeroMQ: [forge].events, [forge].data (streams) and the
// [forge].control command endpoint and the [forge].serial line (c2 frames,
// answered by the application's command registry).
#include "services.hpp"

#include "forge/can.hpp"
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
        serial::LinkOptions o;
        o.device = device;
        o.baud = cfg.value<int>("serial.baud");
        o.framing.frame_size = static_cast<std::size_t>(cfg.value<int>("serial.frame_size"));
        o.thread_name = "forge-serial";
        add_link("serial", device, [o] { return o; });
    }
    const auto interface = cfg.value<std::string>("can.interface");
    if (!interface.empty()) {
        can::IsotpOptions isotp;
        isotp.interface = interface;
        isotp.tx_id = static_cast<std::uint32_t>(cfg.value<std::int64_t>("can.tx_id"));
        isotp.rx_id = static_cast<std::uint32_t>(cfg.value<std::int64_t>("can.rx_id"));
        isotp.extended = cfg.value<bool>("can.extended");
        isotp.fd = cfg.value<bool>("can.fd");
        serial::LinkOptions o;
        o.framing.frame_size = static_cast<std::size_t>(cfg.value<int>("can.frame_size"));
        o.thread_name = "forge-can";
        // A fresh ISO-TP socket for every attempt: the interface may come later.
        add_link("can", can::describe(isotp), [isotp, o] { return can::isotp_link(isotp, o); });
    }
    if (!links_.empty()) app_.every(std::chrono::seconds(1), [this] { check_links(); });
}

void Messaging::add_link(const std::string& name, const std::string& where,
                         std::function<serial::LinkOptions()> options) {
    auto slot = std::make_unique<LinkSlot>();
    slot->name = name;
    slot->where = where;
    slot->options = [this, name, options = std::move(options)] {
        auto o = options();
        o.on_request = [this, name](std::span<const std::uint8_t> frame, const auto& send) {
            CallerScope caller(app_.control().identify("link", name, {}));
            tmtc::Dispatcher(app_.commands()).handle(frame, send);
        };
        return o;
    };
    endpoints_.set(name, where);
    auto& l = *slot;
    links_.push_back(std::move(slot));
    // A device that is not there yet (USB still enumerating, an interface not
    // up) is not fatal: health says so and it is tried again every second.
    open_link(l);
    auto& m = app_.metrics();
    const metrics::Registry::Labels labels{{"link", name}, {"device", where}};
    const auto stat = [&l](auto member) {
        return [&l, member] {
            std::lock_guard lock(l.stats_mutex);
            return static_cast<double>(l.stats.*member);
        };
    };
    const auto framing = [&l](auto member) {
        return [&l, member] {
            std::lock_guard lock(l.stats_mutex);
            return static_cast<double>(l.stats.framing.*member);
        };
    };
    m.gauge_fn("forge.link.requests", labels, stat(&serial::Link::Stats::requests_in), "Commands received on the link");
    m.gauge_fn("forge.link.refused", labels, stat(&serial::Link::Stats::requests_refused),
               "Commands refused because the link was busy");
    m.gauge_fn("forge.serial.messages_received", labels, framing(&serial::Framing::Stats::messages_received),
               "Messages assembled from the line");
    m.gauge_fn("forge.serial.frames_dropped", labels, framing(&serial::Framing::Stats::frames_dropped),
               "Fragments with a bad CRC or COBS");
    m.gauge_fn("forge.serial.messages_dropped", labels, framing(&serial::Framing::Stats::messages_dropped),
               "Messages lost to a missing or out-of-order fragment");
}

bool Messaging::open_link(LinkSlot& l) {
    auto logger = app_.logger();
    try {
        l.link = std::make_unique<serial::Link>(l.options());
    } catch (const std::exception& e) {
        if (l.error != e.what()) {  // the same reason again: only in debug
            logger->warn("forge: cannot open {}: {} (trying again every second)", l.where, e.what());
            l.error = e.what();
        } else {
            logger->debug("forge: cannot open {}: {}", l.where, e.what());
        }
        app_.health().set(l.name, HealthState::failed, e.what(), false);
        return false;
    }
    l.error.clear();
    logger->info("forge: commands on {}", l.where);
    app_.health().set(l.name, HealthState::ok, "commands on " + l.where, false);
    return true;
}

void Messaging::check_links() {
    if (links_stopped_) return;
    for (auto& slot : links_) {
        auto& l = *slot;
        if (l.link) {
            std::lock_guard lock(l.stats_mutex);
            l.stats = l.link->stats();
        }
        if (l.link && !l.link->connected()) {
            app_.logger()->warn("forge: {} went away; waiting for it", l.where);
            app_.health().set(l.name, HealthState::failed, "gone: " + l.where, false);
            l.link.reset();
        }
        if (!l.link) open_link(l);
    }
}

void Messaging::stop_control() {
    links_stopped_ = true;
    if (control_server_) control_server_->stop();
    for (auto& l : links_) l->link.reset();
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
    links_stopped_ = true;
    auto& m = app_.metrics();
    for (const auto& name : {"forge.link.requests", "forge.link.refused"}) m.remove(name, {{"link", "zmq"}});
    for (const auto& l : links_) {
        for (const auto& name : {"forge.link.requests", "forge.link.refused", "forge.serial.messages_received",
                                 "forge.serial.frames_dropped", "forge.serial.messages_dropped"}) {
            m.remove(name, {{"link", l->name}, {"device", l->where}});
        }
    }
    for (const auto& info : stream_list()) {
        for (const auto& name : {"forge.stream.sent", "forge.stream.dropped", "forge.stream.subscribers"}) {
            m.remove(name, {{"topic", info.name}});
        }
    }
    links_.clear();
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
