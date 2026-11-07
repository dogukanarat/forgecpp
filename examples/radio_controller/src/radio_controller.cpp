#include "radio_controller.hpp"

#include "dsp.hpp"
#include "radio/v1/radio.pb.h"

#include <forge/buffer_pool.hpp>
#include <forge/trace.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>

#ifndef RADIO_VERSION
#define RADIO_VERSION "0.0.0"
#endif

using namespace std::chrono_literals;
using forge::json;
using forge::http::HttpError;

namespace radio {
namespace {

constexpr const char* kDeviceAlarm = "radio.device";
constexpr const char* kOverflowAlarm = "radio.rx_overflow";
constexpr const char* kUnderflowAlarm = "radio.tx_underflow";
constexpr const char* kTemperatureAlarm = "radio.temperature";
constexpr int kTapSettleBuffers = 2;  ///< tap buffers skipped after a retune (still queued with the old LO)

std::string channel_list(unsigned mask, const char* prefix) {
    std::string out;
    for (unsigned ch = 0; ch < 2; ++ch) {
        if (mask >> ch & 1u) out += (out.empty() ? "" : ", ") + std::string(prefix) + std::to_string(ch + 1);
    }
    return out.empty() ? "none" : out;
}

json channel_array(unsigned mask) {
    json a = json::array();
    for (unsigned ch = 0; ch < 2; ++ch) {
        if (mask >> ch & 1u) a.push_back(ch + 1);
    }
    return a;
}

/// [radio].<key>: a channel number or a list of them (1 = RX1/TX1, 2 = RX2/TX2) -> bit mask.
unsigned channel_mask(const json& radio, const char* key, unsigned fallback) {
    if (!radio.contains(key)) return fallback;
    const json& v = radio[key];
    unsigned mask = 0;
    for (const auto& c : v.is_array() ? v : json::array({v})) {
        if (!c.is_number_integer() || c.get<int>() < 1 || c.get<int>() > 2) {
            throw forge::ConfigError(std::string("radio.") + key + ": channels are 1 or 2, got " + c.dump());
        }
        mask |= 1u << (c.get<int>() - 1);
    }
    return mask;
}

double round_to(double v, double step) { return std::round(v / step) * step; }

const char* to_string(int mode) {
    switch (mode) {
        case 1: return "streaming";
        case 2: return "cyclic";
        default: return "idle";
    }
}

}  // namespace

RadioController::RadioController()
    : Application({"radio-controller", RADIO_VERSION, "AD9364 radio control and IQ streaming over ZeroMQ"}) {}

std::string RadioController::tx_endpoint() const { return tx_receiver_ ? tx_receiver_->endpoint() : ""; }
std::string RadioController::control_endpoint() const {
    const auto e = const_cast<RadioController*>(this)->endpoints();
    auto it = e.find("control");
    return it == e.end() ? std::string() : it->second;
}
std::string RadioController::data_endpoint() const {
    const auto e = const_cast<RadioController*>(this)->endpoints();
    auto it = e.find("data");
    return it == e.end() ? std::string() : it->second;
}

void RadioController::setup_config(forge::config::Schema& s) {
    using forge::json;
    s.section("radio", "AD936x transceiver");
    s.string("radio.backend", "sim", "sim: simulator, iio: libiio device (also --backend)").choices({"sim", "iio"});
    s.string("radio.uri", "", "libiio context: local:, ip:192.168.2.1, usb:1.2.5 (empty = libiio default)");
    s.integer("radio.timeout_ms", 2000, "libiio timeout").range(100, 60000).unit("ms");
    s.array("radio.rx_channels", json::array({1}), "RX channels it listens to (IQ stream \"iq\"): 1, 2");
    s.integer("radio.tx_channel", 1, "TX channel it drives").range(1, 2);
    s.array("radio.tap_rx", json::array(), "RX channels it shares (topics \"rx1\"/\"rx2\"), e.g. for the analyzer");
    s.integer("radio.tap_samples", 65536, "Samples per shared block (at most buffer_size)").range(1024, 4194304);
    s.number("radio.tap_hz", 20, "Shared blocks per second (0 = every buffer)").range(0, 1000).unit("Hz");
    s.integer("radio.buffer_size", 65536, "Samples per DMA buffer").range(1024, 1 << 22);
    s.boolean("radio.rx_enabled", true, "Stream RX at start");
    s.boolean("radio.tx_enabled", false, "Accept TX frames at start");
    s.number("radio.rx_lo_hz", 2400e6, "RX LO").range(Limits::lo_min_hz, Limits::lo_max_hz).unit("Hz");
    s.number("radio.tx_lo_hz", 2400e6, "TX LO").range(Limits::lo_min_hz, Limits::lo_max_hz).unit("Hz");
    s.number("radio.sample_rate_hz", 30.72e6, "Sample rate").range(Limits::rate_min_hz, Limits::rate_max_hz).unit("Hz");
    s.number("radio.rx_bandwidth_hz", 18e6, "RX RF bandwidth").range(Limits::bw_min_hz, Limits::bw_max_hz).unit("Hz");
    s.number("radio.tx_bandwidth_hz", 18e6, "TX RF bandwidth").range(Limits::bw_min_hz, Limits::bw_max_hz).unit("Hz");
    s.string("radio.gain_mode", "slow_attack", "RX1 gain control").choices(gain_modes());
    s.number("radio.rx_gain_db", 30, "RX1 gain (manual mode)").range(Limits::rx_gain_min_db, Limits::rx_gain_max_db).unit("dB");
    s.string("radio.rx2_gain_mode", "slow_attack", "RX2 gain control").choices(gain_modes());
    s.number("radio.rx2_gain_db", 30, "RX2 gain (manual mode)").range(Limits::rx_gain_min_db, Limits::rx_gain_max_db).unit("dB");
    s.number("radio.tx_attenuation_db", 10, "TX attenuation of tx_channel").range(0, 89.75).unit("dB");
    s.string("radio.state_file", "", "Keep runtime changes across restarts (JSON)");
    s.integer("radio.spectrum_size", 1024, "FFT size of the web spectrum (power of two)").range(64, 65536);
    s.number("radio.spectrum_hz", 5, "Web spectrum updates per second").range(0.5, 30).unit("Hz");
    s.number("radio.temperature_alarm_c", 75, "Temperature alarm threshold").unit("°C");
    s.section("radio.sim", "Simulator");
    s.integer("radio.sim.channels", 1, "1 = AD9364, 2 = AD9361").range(1, 2);
    s.boolean("radio.sim.loopback", true, "Simulated TX -> RX cable");
    s.integer("radio.sim.loopback_tx", 1, "Cable from TX channel").range(1, 2);
    s.integer("radio.sim.loopback_rx", 1, "... to RX channel").range(1, 2);
    s.section("zmq", "Radio sockets");
    s.string("zmq.tx", "tcp://*:5556", "PULL: radio.v1.TxBlock + cs16 samples");
}

void RadioController::setup_cli(CLI::App& cli) {
    cli.add_option("--backend", cli_backend_, "Radio backend: sim | iio (overrides [radio].backend)")
        ->check(CLI::IsMember({"sim", "iio"}));
    cli.add_option("--uri", cli_uri_, "libiio context URI, e.g. ip:192.168.2.1, local:, usb:1.2.5");
}

// ---------------------------------------------------------------------------
// Controls: web panel, custom page, console and ZeroMQ all use these.

void RadioController::setup_web(forge::web::Dashboard& ui) {
    load_settings(config());
    ui.title("Radio controller");
    ui.link("Spectrum", "/");

    auto& rx = ui.group("Receiver");
    rx.toggle("rx_enabled", "RX streaming", [this] { return rx_enabled_.load(); },
              [this](bool on) {
                  rx_enabled_ = on;
                  logger()->info("RX streaming {}", on ? "enabled" : "disabled");
                  if (!on && backend_) backend_->cancel_rx();
                  mark_dirty();
              });
    rx.number("rx_lo_mhz", "RX LO", [this] { return settings_.rx_lo_hz / 1e6; },
              [this](double mhz) {
                  const double hz = round_to(mhz * 1e6, 1);
                  control("RX LO", [&] { backend_->set_rx_lo(hz); });
                  settings_.rx_lo_hz = hz;
                  rx_lo_hz_ = hz;
                  ++tuning_version_;
              })
        .range(Limits::lo_min_hz / 1e6, Limits::lo_max_hz / 1e6, 0.001)
        .unit("MHz")
        .precision(3);
    rx.number("sample_rate_msps", "Sample rate", [this] { return settings_.sample_rate_hz / 1e6; },
              [this](double msps) {
                  const double hz = round_to(msps * 1e6, 1);
                  control("sample rate", [&] { backend_->set_sample_rate(hz); });
                  settings_.sample_rate_hz = hz;
                  rate_hz_ = hz;
                  ++tuning_version_;
              })
        .range(Limits::rate_min_hz / 1e6, Limits::rate_max_hz / 1e6, 0.001)
        .unit("MSPS")
        .precision(3);
    rx.number("rx_bw_mhz", "RX bandwidth", [this] { return settings_.rx_bandwidth_hz / 1e6; },
              [this](double mhz) {
                  const double hz = round_to(mhz * 1e6, 1);
                  control("RX bandwidth", [&] { backend_->set_rx_bandwidth(hz); });
                  settings_.rx_bandwidth_hz = hz;
                  ++tuning_version_;
              })
        .range(Limits::bw_min_hz / 1e6, Limits::bw_max_hz / 1e6, 0.001)
        .unit("MHz")
        .precision(3);
    rx.select("gain_mode", "RX1 gain control", gain_modes(), [this] { return settings_.gain_mode; },
              [this](const std::string& mode) {
                  control("gain mode", [&] { backend_->set_gain_mode(mode); });
                  settings_.gain_mode = mode;
              });
    rx.slider("rx_gain_db", "RX1 gain", [this] { return rx_gain_actual_[0].load(); },
              [this](double db) {
                  if (settings_.gain_mode != "manual") {
                      throw HttpError(409, "RX gain is controlled by the AGC; set gain_mode to manual first");
                  }
                  control("RX gain", [&] { backend_->set_rx_gain(db); });
                  settings_.rx_gain_db = db;
                  rx_gain_actual_[0] = db;
              },
              Limits::rx_gain_min_db, Limits::rx_gain_max_db, 1)
        .unit("dB")
        .precision(0)
        .help("Writable in manual gain mode; shows the AGC gain otherwise");
    rx.value("rx_power_dbfs", "RX power", rx_power_dbfs_).unit("dBFS").precision(1);
    rx.value("peak", "Strongest signal", [this] {
        return fmt::format("{:+.1f} kHz @ {:.1f} dBFS", peak_offset_khz_, peak_dbfs_);
    });
    rx.value("rx_rate_msps", "Measured rate", rx_rate_msps_).unit("MSPS").precision(3);
    rx.value("rx_overflows", "Overflows", [this] { return rx_overflows_.load(); });
    rx.value("subscriptions", "IQ subscribers", [this] { return subscriptions_.load(); });

    auto& rx2 = ui.group("RX2");
    rx2.select("rx2_gain_mode", "RX2 gain control", gain_modes(), [this] { return settings_.rx2_gain_mode; },
               [this](const std::string& mode) {
                   control("RX2 gain mode", [&] { backend_->set_gain_mode(mode, 1); });
                   settings_.rx2_gain_mode = mode;
               })
        .help("AD9361/AD9363: RX2 shares the RX LO and sample rate with RX1 and has its own gain");
    rx2.slider("rx2_gain_db", "RX2 gain", [this] { return rx_gain_actual_[1].load(); },
               [this](double db) {
                   if (settings_.rx2_gain_mode != "manual") {
                       throw HttpError(409, "RX2 gain is controlled by the AGC; set rx2_gain_mode to manual first");
                   }
                   control("RX2 gain", [&] { backend_->set_rx_gain(db, 1); });
                   settings_.rx2_gain_db = db;
                   rx_gain_actual_[1] = db;
               },
               Limits::rx_gain_min_db, Limits::rx_gain_max_db, 1)
        .unit("dB")
        .precision(0);

    auto& roles = ui.group("Channels");
    roles.value("hw_channels", "Transceiver", [this] {
        return connected_ ? fmt::format("{}R{}T", hw_rx_channels_.load(), hw_tx_channels_.load()) : std::string("-");
    });
    roles.value("rx_channels", "Listens to (IQ stream)", [this] { return channel_array(rx_mask_); });
    roles.value("tx_channel", "Drives", [this] { return fmt::format("TX{}", tx_channel_ + 1); });
    roles.value("tap_channels", "Shares (tap)", [this] { return channel_array(tap_mask_); });
    roles.value("tap_blocks", "Tap blocks sent", [this] { return tap_blocks_.load(); });
    roles.value("tap_subscriptions", "Tap subscribers", [this] { return tap_subscriptions_.load(); });

    auto& tx = ui.group("Transmitter");
    tx.toggle("tx_enabled", "TX enabled", [this] { return tx_enabled_.load(); },
              [this](bool on) {
                  tx_enabled_ = on;
                  if (!on && backend_) {
                      backend_->stop_tx();
                      tx_mode_ = TxMode::idle;
                      tx_reset_ = true;
                  }
                  logger()->info("TX {}", on ? "enabled" : "disabled");
                  mark_dirty();
              })
        .help("Frames received while disabled are dropped");
    tx.number("tx_lo_mhz", "TX LO", [this] { return settings_.tx_lo_hz / 1e6; },
              [this](double mhz) {
                  const double hz = round_to(mhz * 1e6, 1);
                  control("TX LO", [&] { backend_->set_tx_lo(hz); });
                  settings_.tx_lo_hz = hz;
              })
        .range(Limits::lo_min_hz / 1e6, Limits::lo_max_hz / 1e6, 0.001)
        .unit("MHz")
        .precision(3);
    tx.number("tx_bw_mhz", "TX bandwidth", [this] { return settings_.tx_bandwidth_hz / 1e6; },
              [this](double mhz) {
                  const double hz = round_to(mhz * 1e6, 1);
                  control("TX bandwidth", [&] { backend_->set_tx_bandwidth(hz); });
                  settings_.tx_bandwidth_hz = hz;
              })
        .range(Limits::bw_min_hz / 1e6, Limits::bw_max_hz / 1e6, 0.001)
        .unit("MHz")
        .precision(3);
    tx.slider("tx_atten_db", "TX attenuation", [this] { return settings_.tx_attenuation_db; },
              [this](double db) {
                  control("TX attenuation", [&] { backend_->set_tx_attenuation(db, tx_channel_); });
                  settings_.tx_attenuation_db = round_to(db, 0.25);
              },
              Limits::tx_atten_min_db, Limits::tx_atten_max_db, 0.25)
        .unit("dB")
        .precision(2);
    tx.value("tx_mode", "Output", [this] { return to_string(static_cast<int>(tx_mode_.load())); });
    tx.value("tx_frames", "Frames", [this] { return tx_frames_.load(); });
    tx.value("tx_underflows", "Underflows", [this] { return tx_underflows_.load(); });
    tx.value("tx_dropped", "Dropped (TX disabled)", [this] { return tx_dropped_.load(); });
    tx.button("tx_stop", "Stop output", [this] {
          if (backend_) backend_->stop_tx();
          tx_mode_ = TxMode::idle;
          tx_reset_ = true;
          logger()->info("TX output stopped");
      })
        .danger();

    auto& dev = ui.group("Device");
    dev.value("connected", "Connected", connected_);
    dev.value("backend", "Backend", [this] {
        std::shared_lock lock(backend_mutex_);
        return backend_ ? backend_->name() : backend_kind_ + " (not connected)";
    });
    dev.value("model", "Model", [this] {
        if (device_info_.contains("attrs") && device_info_["attrs"].contains("hw_model")) {
            return device_info_["attrs"]["hw_model"].get<std::string>();
        }
        return device_info_.value("model", std::string("AD936x"));
    });
    dev.value("temperature_c", "Temperature", [this] { return temperature_ ? json(*temperature_) : json(nullptr); })
        .unit("°C")
        .precision(1);
    dev.number("buffer_size", "Buffer size", [this] { return double(buffer_size_.load()); },
               [this](double v) {
                   buffer_size_ = static_cast<std::size_t>(v);
                   restart_rx_ = true;
                   tx_reset_ = true;
                   std::shared_lock lock(backend_mutex_);
                   if (backend_) backend_->cancel_rx();
                   mark_dirty();
               })
        .range(1024, 1 << 20, 1024)
        .unit("samples");
    dev.button("reconnect", "Reconnect", [this] {
           close_backend();
           open_backend();
       })
        .confirm("Reopen the radio? Streaming restarts.");

    auto& net = ui.group("ZeroMQ");
    net.value("zmq_data", "Streams (PUB: iq, rx1, rx2)", [this] { return data_endpoint(); });
    net.value("zmq_tx", "TX input (PULL)", [this] { return tx_endpoint(); });
    net.value("zmq_control", "Control (forge.v1)", [this] { return control_endpoint(); });
}

template <class F>
void RadioController::control(const char* what, F&& fn) {
    if (!backend_) throw HttpError(503, std::string("radio not connected; cannot change ") + what);
    try {
        fn();
    } catch (const DeviceError& e) {
        throw HttpError(502, e.what());
    }
    mark_dirty();
}

// ---------------------------------------------------------------------------
// Lifecycle

void RadioController::load_settings(const forge::Config& cfg) {
    backend_kind_ = cli_backend_.empty() ? cfg.get_or<std::string>("radio.backend", "sim") : cli_backend_;
    uri_ = cli_uri_.empty() ? cfg.get_or<std::string>("radio.uri", "") : cli_uri_;
    timeout_ms_ = cfg.get_or<unsigned>("radio.timeout_ms", 2000);
    state_file_ = cfg.get_or<std::string>("radio.state_file", "");
    spectrum_size_ = cfg.get_or<std::size_t>("radio.spectrum_size", 1024);
    if (!dsp::is_power_of_two(spectrum_size_)) throw forge::ConfigError("radio.spectrum_size must be a power of two");
    temperature_alarm_c_ = cfg.get_or<double>("radio.temperature_alarm_c", 75);
    sim_.loopback = cfg.get_or<bool>("radio.sim.loopback", true);
    sim_.channels = std::clamp(cfg.get_or<unsigned>("radio.sim.channels", 1), 1u, 2u);
    sim_.loopback_tx = std::clamp(cfg.get_or<unsigned>("radio.sim.loopback_tx", 1), 1u, 2u) - 1;
    sim_.loopback_rx = std::clamp(cfg.get_or<unsigned>("radio.sim.loopback_rx", 1), 1u, 2u) - 1;

    // Channel roles (fixed at start-up; must fit the transceiver)
    const json radio = cfg.section("radio").to_json();
    rx_mask_ = channel_mask(radio, "rx_channels", Backend::kRx1);
    tap_mask_ = channel_mask(radio, "tap_rx", 0);
    const unsigned tx = channel_mask(radio, "tx_channel", 1);
    if (tx != 1 && tx != 2) throw forge::ConfigError("radio.tx_channel is one channel: 1 or 2");
    tx_channel_ = tx == 1 ? 0 : 1;
    tap_hz_ = std::clamp(cfg.get_or<double>("radio.tap_hz", 20.0), 0.0, 1000.0);
    tap_samples_ = cfg.get_or<std::size_t>("radio.tap_samples", 65536);
    if (tap_samples_ < 1024 || tap_samples_ > (1u << 22)) {
        throw forge::ConfigError("radio.tap_samples must be between 1024 and 4194304");
    }

    json j = cfg.section("radio").to_json();
    // The state file (last values set at runtime) wins over the config file.
    if (!state_file_.empty() && std::filesystem::exists(state_file_)) {
        try {
            const auto saved = forge::load_json(state_file_);
            j.update(saved);
            logger()->info("restored radio settings from {}", state_file_);
        } catch (const std::exception& e) {
            logger()->warn("ignoring {}: {}", state_file_, e.what());
        }
    }
    settings_ = settings_from_json(j, Settings{});
    settings_.tx_channel = tx_channel_;
    buffer_size_ = j.value("buffer_size", std::size_t{65536});
    rx_enabled_ = j.value("rx_enabled", true);
    tx_enabled_ = j.value("tx_enabled", false);
    rx_lo_hz_ = settings_.rx_lo_hz;
    rate_hz_ = settings_.sample_rate_hz;
}

void RadioController::on_start() {
    const auto cfg = config();

    // Streams on [forge].data, so clients can subscribe even while the radio is absent.
    iq_stream_ = &stream("iq", "radio.v1.IqBlock");
    for (unsigned ch = 0; ch < 2; ++ch) {
        if (tap_mask_ >> ch & 1u) {
            tap_stream_[ch] = &stream("rx" + std::to_string(ch + 1), "radio.v1.IqBlock",
                                      forge::zmq::StreamOptions{"latest", tap_hz_});
        }
    }
    running_ = true;
    rx_thread_ = std::thread([this] { rx_loop(); });

    forge::zmq::Receiver::Options tx_opts;
    tx_opts.bind = true;
    tx_opts.socket.recv_hwm = 64;  // backpressure towards fast senders
    tx_receiver_ = std::make_unique<forge::zmq::Receiver>(
        forge::zmq::Receiver::Kind::pull, cfg.get_or<std::string>("zmq.tx", "tcp://*:5556"),
        [this](forge::zmq::Message&& m) { on_tx_frame(std::move(m)); }, tx_opts);
    logger()->info("zmq: streams on {}, TX input on {}", data_endpoint(), tx_endpoint());
    logger()->info("channels: listens to {}, drives TX{}, shares {}", channel_list(rx_mask_, "RX"), tx_channel_ + 1,
                   channel_list(tap_mask_, "RX"));

    open_backend();

    every(500ms, [this] { telemetry(); });
    const double spectrum_hz = std::clamp(cfg.get_or<double>("radio.spectrum_hz", 5.0), 0.5, 30.0);
    every(std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / spectrum_hz)),
          [this] { update_spectrum(); });
    every(5s, [this] {
        if (!connected_) open_backend();  // automatic reconnect
    });
    every(1s, [this] { save_state(); });
}

void RadioController::on_stop() {
    running_ = false;
    {
        std::shared_lock lock(backend_mutex_);
        if (backend_) backend_->cancel_rx();
    }
    if (rx_thread_.joinable()) rx_thread_.join();
    tx_receiver_.reset();
    close_backend();
    dirty_ = true;
    save_state();
}

void RadioController::on_reload(const forge::Config& cfg) {
    const auto old_kind = backend_kind_;
    const auto old_uri = uri_;
    // Channel roles are fixed while streaming (the RX thread uses them): restart to change them.
    const auto rx = rx_mask_, tap = tap_mask_, tx = tx_channel_;
    const auto tap_samples = tap_samples_;
    const auto tap_hz = tap_hz_;
    load_settings(cfg);
    if (rx_mask_ != rx || tap_mask_ != tap || tx_channel_ != tx) {
        logger()->warn("channel roles (rx_channels, tap_rx, tx_channel) take effect at the next start");
    }
    rx_mask_ = rx;
    tap_mask_ = tap;
    tx_channel_ = tx;
    settings_.tx_channel = tx;
    tap_samples_ = tap_samples;
    tap_hz_ = tap_hz;
    if (backend_kind_ != old_kind || uri_ != old_uri) {
        close_backend();
        open_backend();
    } else if (backend_) {
        try {
            backend_->apply(settings_);
            ++tuning_version_;
        } catch (const DeviceError& e) {
            device_lost(e.what());
        }
    }
}

void RadioController::open_backend() {
    if (backend_) return;
    std::unique_ptr<Backend> b;
    try {
        if (backend_kind_ == "iio") {
            b = make_iio_backend(uri_, timeout_ms_);
        } else {
            b = make_sim_backend(sim_);
        }
        // The configured roles must fit the hardware (AD9364: RX1/TX1 only).
        const unsigned rx_need = (rx_mask_ | tap_mask_) & Backend::kRx2 ? 2 : 1;
        if (rx_need > b->rx_channel_count() || tx_channel_ + 1 > b->tx_channel_count()) {
            throw DeviceError(fmt::format(
                "the configuration uses {} and TX{} but the transceiver has {} RX / {} TX channel(s); "
                "set [radio] rx_channels, tap_rx and tx_channel to channels it has (RX2/TX2 need an "
                "AD9361/AD9363 in 2R2T mode)",
                channel_list(rx_mask_ | tap_mask_, "RX"), tx_channel_ + 1, b->rx_channel_count(),
                b->tx_channel_count()));
        }
        b->apply(settings_);
        rx_gain_actual_[0] = b->rx_gain(0);
        if (b->rx_channel_count() > 1) rx_gain_actual_[1] = b->rx_gain(1);
        hw_rx_channels_ = b->rx_channel_count();
        hw_tx_channels_ = b->tx_channel_count();
        device_info_ = b->describe();
    } catch (const DeviceError& e) {
        if (connected_ || !alarms().active(kDeviceAlarm)) logger()->error("radio: {}", e.what());
        connected_ = false;
        alarms().raise(kDeviceAlarm, forge::Severity::error, std::string("Radio not available: ") + e.what());
        health().set("radio", forge::HealthState::failed, e.what());
        return;
    }
    {
        std::unique_lock lock(backend_mutex_);
        backend_ = std::move(b);
        ++generation_;
    }
    connected_ = true;
    tx_mode_ = TxMode::idle;
    tx_reset_ = true;
    alarms().clear(kDeviceAlarm);
    health().set("radio", forge::HealthState::ok, backend_->name());
    ++tuning_version_;
    logger()->info("radio: connected to {} ({}R{}T; RX {:.3f} MHz, {:.3f} MSPS)", backend_->name(),
                   hw_rx_channels_.load(), hw_tx_channels_.load(), settings_.rx_lo_hz / 1e6,
                   settings_.sample_rate_hz / 1e6);
}

void RadioController::close_backend() {
    std::unique_ptr<Backend> old;
    {
        // Unblock the RX thread before taking the exclusive lock.
        std::shared_lock lock(backend_mutex_);
        if (backend_) backend_->cancel_rx();
    }
    {
        std::unique_lock lock(backend_mutex_);
        old = std::move(backend_);
        ++generation_;
    }
    if (old) {
        old->stop_tx();
        old.reset();
        logger()->info("radio: disconnected");
    }
    connected_ = false;
    tx_mode_ = TxMode::idle;
}

void RadioController::device_lost(const std::string& why) {
    if (!connected_) return;
    logger()->error("radio: device error: {}", why);
    alarms().raise(kDeviceAlarm, forge::Severity::error, "Radio connection lost: " + why);
    health().set("radio", forge::HealthState::failed, why);
    close_backend();  // the 5 s timer reconnects
}

// ---------------------------------------------------------------------------
// RX thread: device -> ZeroMQ PUB

void RadioController::rx_loop() {
    configure_thread("rx");  // [threads.rx]: priority, CPU (real-time receive)
    auto& frames = metrics().counter("radio.rx.buffers", "RX DMA buffers read");
    auto& samples_metric = metrics().counter("radio.rx.samples", "RX samples read (per channel)");
    auto& read_time = metrics().histogram("radio.rx.read_seconds", forge::metrics::latency_buckets(),
                                          "Time blocked in read_rx (waiting for the DMA)", "s");

    // Buffers recycle through a pool: published blocks go out without copying
    // and come back when ZeroMQ has sent them.
    std::shared_ptr<forge::BufferPool<std::int16_t>> pool;
    std::uint64_t seq = 0;
    std::uint64_t generation_seen = ~0ull;
    bool streaming = false;
    auto next_snapshot = std::chrono::steady_clock::now();
    std::uint64_t tuning_seen = ~0ull;
    int settle = 0;

    while (running_) {
        subscriptions_ = iq_stream_->subscribers();
        std::size_t tap_subs = 0;
        for (auto* t : tap_stream_) tap_subs += t ? t->subscribers() : 0;
        tap_subscriptions_ = tap_subs;
        std::shared_lock lock(backend_mutex_);
        Backend* b = backend_.get();

        if (b && (generation_ != generation_seen || restart_rx_.exchange(false))) {
            if (streaming && generation_ == generation_seen) b->stop_rx();
            streaming = false;
            generation_seen = generation_;
        }
        const unsigned mask = rx_mask_ | tap_mask_;  // channels in the DMA buffer
        if (!b || !rx_enabled_ || mask == 0) {
            if (streaming && b) b->stop_rx();
            streaming = false;
            lock.unlock();
            std::this_thread::sleep_for(50ms);
            continue;
        }

        const std::size_t size = buffer_size_;
        std::size_t n = 0;
        std::shared_ptr<std::vector<std::int16_t>> buf[2];
        // Shared channels: one block per tap interval (Stream::wanted() holds the rate limit).
        bool tap_due[2] = {false, false};
        try {
            if (!streaming) {
                pool = forge::BufferPool<std::int16_t>::create(2 * size, 8, 32);
                b->start_rx(size, mask);
                streaming = true;
            }
            // Buffers queued before a retune carry the old LO: shared blocks skip them.
            if (const auto tv = tuning_version_.load(); tv != tuning_seen) {
                tuning_seen = tv;
                settle = kTapSettleBuffers;
            }
            for (unsigned ch = 0; ch < 2; ++ch) {
                if (settle == 0 && tap_stream_[ch]) tap_due[ch] = tap_stream_[ch]->wanted();
                // Own channels are always converted (stream, spectrum); shared ones only when due.
                if ((rx_mask_ >> ch & 1u) || tap_due[ch]) buf[ch] = pool->acquire();
            }
            {
                FORGE_TRACE_SCOPE("rx.read", "radio");
                forge::metrics::Timer t(read_time);
                n = b->read_rx(buf[0] ? buf[0]->data() : nullptr, buf[1] ? buf[1]->data() : nullptr, size);
            }
            if (b->take_rx_overflow()) ++rx_overflows_;
        } catch (const DeviceError& e) {
            streaming = false;
            lock.unlock();
            post([this, why = std::string(e.what())] { device_lost(why); });
            std::this_thread::sleep_for(500ms);
            continue;
        }
        lock.unlock();
        if (n == 0) {
            // Cancelled (reconfiguration or shutdown): back off so the main
            // loop can take the exclusive lock (glibc rwlocks favour readers).
            std::this_thread::sleep_for(5ms);
            continue;
        }
        if (settle > 0) --settle;

        ++rx_frames_;
        rx_samples_ += n;
        frames.inc();
        samples_metric.inc(n);
        ++seq;

        FORGE_TRACE_SCOPE("rx.publish", "radio");
        auto block = [&](unsigned ch, std::size_t count) {
            radio::v1::IqBlock m;
            m.set_channel(ch + 1);
            m.set_sample_rate(rate_hz_.load());
            m.set_center_freq(rx_lo_hz_.load());
            m.set_samples(static_cast<std::uint32_t>(count));
            m.set_format("cs16");
            m.set_rx_gain_db(rx_gain_actual_[ch].load());
            m.set_buffer(seq);
            return m;
        };
        // Own channels: topic "iq", every buffer while someone listens.
        if (subscriptions_ > 0) {
            for (unsigned ch = 0; ch < 2; ++ch) {
                if (!(rx_mask_ >> ch & 1u)) continue;
                iq_stream_->publish(block(ch, n), buf[ch], buf[ch]->data(), n * 2 * sizeof(std::int16_t));
            }
        }
        // Shared channels: the start of this buffer (contiguous even when samples were lost).
        for (unsigned ch = 0; ch < 2; ++ch) {
            if (!tap_due[ch] || !buf[ch]) continue;
            const std::size_t count = std::min(n, tap_samples_);
            tap_stream_[ch]->publish(block(ch, count), buf[ch], buf[ch]->data(), count * 2 * sizeof(std::int16_t));
            ++tap_blocks_;
        }
        const auto now = std::chrono::steady_clock::now();
        const unsigned first = (rx_mask_ & Backend::kRx1) ? 0u : 1u;  // spectrum of the first own channel
        if (rx_mask_ && buf[first] && now >= next_snapshot && n >= spectrum_size_) {
            std::lock_guard l(snapshot_mutex_);
            snapshot_.assign(buf[first]->begin(), buf[first]->begin() + static_cast<std::ptrdiff_t>(2 * spectrum_size_));
            next_snapshot = now + 50ms;
        }
    }
    {
        std::shared_lock lock(backend_mutex_);
        if (streaming && backend_) backend_->stop_rx();
    }
}

// ---------------------------------------------------------------------------
// TX thread (ZeroMQ PULL receiver): clients -> device

void RadioController::on_tx_frame(forge::zmq::Message&& m) {
    if (!tx_enabled_) {
        ++tx_dropped_;
        return;
    }
    radio::v1::TxBlock tx;
    try {
        tx = m.as<radio::v1::TxBlock>();
    } catch (const std::exception& e) {
        ++tx_dropped_;
        logger()->warn("radio: TX frame ignored: {}", e.what());
        return;
    }
    if ((!tx.format().empty() && tx.format() != "cs16") || m.blob.size() % 4 != 0 || m.blob.empty()) {
        ++tx_dropped_;
        logger()->warn("radio: TX frame ignored (need format cs16 and a blob of int16 I/Q pairs)");
        return;
    }
    FORGE_TRACE_SCOPE("tx.write", "radio");
    const auto* samples = reinterpret_cast<const std::int16_t*>(m.blob.data());
    const std::size_t n = m.blob.size() / 4;
    const bool cyclic = tx.cyclic();

    std::shared_lock lock(backend_mutex_);
    Backend* b = backend_.get();
    if (!b) {
        ++tx_dropped_;
        return;
    }
    if (tx_generation_ != generation_ || tx_reset_.exchange(false)) {
        tx_generation_ = generation_;
        tx_pending_.clear();
        if (tx_mode_ != TxMode::idle) tx_mode_ = TxMode::idle;
    }
    try {
        if (cyclic) {
            // A waveform the hardware repeats until stopped (e.g. a test tone).
            b->start_tx(n, true, tx_channel_);
            b->write_tx(samples, n);
            tx_mode_ = TxMode::cyclic;
        } else {
            const std::size_t size = buffer_size_;
            if (tx_mode_ != TxMode::streaming) {
                b->start_tx(size, false, tx_channel_);
                tx_mode_ = TxMode::streaming;
            }
            tx_pending_.insert(tx_pending_.end(), samples, samples + 2 * n);
            std::size_t offset = 0;
            while (tx_pending_.size() - offset >= 2 * size) {
                b->write_tx(tx_pending_.data() + offset, size);
                offset += 2 * size;
            }
            tx_pending_.erase(tx_pending_.begin(), tx_pending_.begin() + static_cast<std::ptrdiff_t>(offset));
        }
        if (b->take_tx_underflow()) ++tx_underflows_;
        ++tx_frames_;
        tx_samples_ += n;
    } catch (const DeviceError& e) {
        lock.unlock();
        post([this, why = std::string(e.what())] { device_lost(why); });
    }
}

// ---------------------------------------------------------------------------
// Main loop timers

void RadioController::telemetry() {
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - last_telemetry_).count();
    last_telemetry_ = now;
    const auto samples = rx_samples_.load();
    if (dt > 0) rx_rate_msps_ = static_cast<double>(samples - last_rx_samples_) / dt / 1e6;
    last_rx_samples_ = samples;

    if (backend_) {
        try {
            rx_gain_actual_[0] = backend_->rx_gain(0);
            if (hw_rx_channels_ > 1) rx_gain_actual_[1] = backend_->rx_gain(1);
            temperature_ = backend_->temperature_c();
        } catch (const DeviceError& e) {
            device_lost(e.what());
        }
    }

    // Overflow alarm: raised while overflows keep happening, cleared after 5 s without.
    const auto overflows = rx_overflows_.load();
    if (overflows != last_rx_overflows_) {
        last_overflow_ = now;
        alarms().raise(kOverflowAlarm, forge::Severity::warning,
                       fmt::format("RX overflow: samples lost ({} total); lower the sample rate or speed up clients",
                                   overflows));
    } else if (now - last_overflow_ > 5s) {
        alarms().clear(kOverflowAlarm);
    }
    last_rx_overflows_ = overflows;

    const auto underflows = tx_underflows_.load();
    alarms().set(kUnderflowAlarm, underflows != last_tx_underflows_, forge::Severity::warning,
                 fmt::format("TX underflow: the sender is not keeping up ({} total)", underflows));
    last_tx_underflows_ = underflows;

    if (temperature_) {
        if (*temperature_ > temperature_alarm_c_) {
            alarms().raise(kTemperatureAlarm, forge::Severity::warning,
                           fmt::format("Transceiver temperature {:.1f} °C above {:.0f} °C", *temperature_,
                                       temperature_alarm_c_));
        } else if (*temperature_ < temperature_alarm_c_ - 3) {
            alarms().clear(kTemperatureAlarm);
        }
    }
}

void RadioController::update_spectrum() {
    std::vector<std::int16_t> snap;
    {
        std::lock_guard l(snapshot_mutex_);
        snap.swap(snapshot_);
    }
    if (snap.size() < 2 * spectrum_size_) return;
    const auto spectrum = dsp::spectrum_dbfs(snap.data(), spectrum_size_);
    rx_power_dbfs_ = dsp::power_dbfs(snap.data(), spectrum_size_);
    const auto peak = dsp::find_peak(spectrum, settings_.sample_rate_hz);
    peak_offset_khz_ = peak.offset_hz / 1e3;
    peak_dbfs_ = peak.dbfs;

    json bins = json::array();
    for (float v : spectrum) bins.push_back(std::round(v * 10) / 10);
    publish_web("spectrum", {{"bins", std::move(bins)},
                             {"center_freq", settings_.rx_lo_hz},
                             {"sample_rate", settings_.sample_rate_hz},
                             {"power_dbfs", rx_power_dbfs_}});
}

void RadioController::save_state() {
    if (!dirty_ || state_file_.empty()) return;
    dirty_ = false;
    auto j = to_json(settings_);
    j["buffer_size"] = buffer_size_.load();
    j["rx_enabled"] = rx_enabled_.load();
    j["tx_enabled"] = tx_enabled_.load();
    try {
        forge::save_json(state_file_, j);
    } catch (const std::exception& e) {
        logger()->warn("cannot save {}: {}", state_file_, e.what());
    }
}

}  // namespace radio
