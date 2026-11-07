#include "radio_controller.hpp"

#include "dsp.hpp"

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
constexpr const char* kRx2Alarm = "radio.rx2";
constexpr int kRx2SettleBuffers = 2;  ///< RX2 buffers skipped after a retune (still queued with the old LO)

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
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
std::string RadioController::control_endpoint() const { return control_server_ ? control_server_->endpoint() : ""; }

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
    rx.select("gain_mode", "Gain control", gain_modes(), [this] { return settings_.gain_mode; },
              [this](const std::string& mode) {
                  control("gain mode", [&] { backend_->set_gain_mode(mode); });
                  settings_.gain_mode = mode;
              });
    rx.slider("rx_gain_db", "RX gain", [this] { return rx_gain_actual_; },
              [this](double db) {
                  if (settings_.gain_mode != "manual") {
                      throw HttpError(409, "RX gain is controlled by the AGC; set gain_mode to manual first");
                  }
                  control("RX gain", [&] { backend_->set_rx_gain(db); });
                  settings_.rx_gain_db = db;
                  rx_gain_actual_ = db;
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
    rx2.toggle("rx2_enabled", "RX2", [this] { return rx2_enabled_.load(); },
               [this](bool on) {
                   if (on && connected_ && rx_channel_count_ < 2) {
                       throw HttpError(409, "this transceiver has one RX channel (AD9364); RX2 needs an AD9361/AD9363");
                   }
                   rx2_enabled_ = on;
                   restart_rx_ = true;  // the DMA buffer carries both channels
                   std::shared_lock lock(backend_mutex_);
                   if (backend_) backend_->cancel_rx();
                   logger()->info("RX2 {}", on ? "enabled" : "disabled");
                   mark_dirty();
               })
        .help("AD9361/AD9363: RX2 shares the RX LO and sample rate with RX1; its samples go to the RX2 socket "
              "(spectrum analyzer). Toggling restarts RX streaming.");
    rx2.select("rx2_gain_mode", "RX2 gain control", gain_modes(), [this] { return settings_.rx2_gain_mode; },
               [this](const std::string& mode) {
                   control("RX2 gain mode", [&] { backend_->set_gain_mode(mode, 1); });
                   settings_.rx2_gain_mode = mode;
               });
    rx2.slider("rx2_gain_db", "RX2 gain", [this] { return rx2_gain_actual_.load(); },
               [this](double db) {
                   if (settings_.rx2_gain_mode != "manual") {
                       throw HttpError(409, "RX2 gain is controlled by the AGC; set rx2_gain_mode to manual first");
                   }
                   control("RX2 gain", [&] { backend_->set_rx_gain(db, 1); });
                   settings_.rx2_gain_db = db;
                   rx2_gain_actual_ = db;
               },
               Limits::rx_gain_min_db, Limits::rx_gain_max_db, 1)
        .unit("dB")
        .precision(0);
    rx2.value("rx2_blocks", "Blocks sent", [this] { return rx2_blocks_.load(); });
    rx2.value("rx2_subscriptions", "RX2 subscribers", [this] { return rx2_subscriptions_.load(); });

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
                  control("TX attenuation", [&] { backend_->set_tx_attenuation(db); });
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
    dev.value("rx_channels", "RX channels", [this] { return rx_channel_count_.load(); });
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
    net.value("zmq_rx", "IQ stream (PUB)", rx_endpoint_);
    net.value("zmq_rx2", "RX2 blocks (PUB)", rx2_endpoint_);
    net.value("zmq_tx", "TX input (PULL)", [this] { return tx_endpoint(); });
    net.value("zmq_control", "Control (REP)", [this] { return control_endpoint(); });
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
    sim_loopback_ = cfg.get_or<bool>("radio.sim.loopback", true);
    sim_rx_channels_ = std::clamp(cfg.get_or<unsigned>("radio.sim.rx_channels", 1), 1u, 2u);
    sim_loopback_rx_ = std::clamp(cfg.get_or<unsigned>("radio.sim.loopback_rx", 1), 1u, 2u) - 1;
    rx2_tap_hz_ = std::clamp(cfg.get_or<double>("radio.rx2_tap_hz", 20.0), 0.0, 1000.0);
    rx2_tap_samples_ = cfg.get_or<std::size_t>("radio.rx2_tap_samples", 65536);
    if (rx2_tap_samples_ < 1024 || rx2_tap_samples_ > (1u << 22)) {
        throw forge::ConfigError("radio.rx2_tap_samples must be between 1024 and 4194304");
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
    buffer_size_ = j.value("buffer_size", std::size_t{65536});
    rx_enabled_ = j.value("rx_enabled", true);
    tx_enabled_ = j.value("tx_enabled", false);
    rx2_enabled_ = j.value("rx2_enabled", false);
    rx_lo_hz_ = settings_.rx_lo_hz;
    rate_hz_ = settings_.sample_rate_hz;
}

void RadioController::on_start() {
    const auto cfg = config();

    // Sockets first, so clients can connect even while the radio is absent.
    running_ = true;
    std::promise<Endpoints> bound;
    auto rx_bound = bound.get_future();
    rx_thread_ = std::thread([this, p = std::move(bound)]() mutable { rx_loop(std::move(p)); });
    const auto endpoints = rx_bound.get();
    rx_endpoint_ = endpoints.rx;
    rx2_endpoint_ = endpoints.rx2;

    forge::zmq::Receiver::Options tx_opts;
    tx_opts.bind = true;
    tx_opts.socket.recv_hwm = 64;  // backpressure towards fast senders
    tx_receiver_ = std::make_unique<forge::zmq::Receiver>(
        forge::zmq::Receiver::Kind::pull, cfg.get_or<std::string>("zmq.tx", "tcp://*:5556"),
        [this](forge::zmq::Message&& m) { on_tx_frame(std::move(m)); }, tx_opts);
    control_server_ = forge::zmq::serve_application(*this, cfg.get_or<std::string>("zmq.control", "tcp://*:5557"));
    logger()->info("zmq: IQ stream on {}, RX2 blocks on {}, TX input on {}, control on {}", rx_endpoint_,
                   rx2_endpoint_, tx_endpoint(), control_endpoint());

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
    control_server_.reset();
    close_backend();
    dirty_ = true;
    save_state();
}

void RadioController::on_reload(const forge::Config& cfg) {
    const auto old_kind = backend_kind_;
    const auto old_uri = uri_;
    load_settings(cfg);
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
            SimOptions opts;
            opts.loopback = sim_loopback_;
            opts.loopback_rx = sim_loopback_rx_;
            opts.rx_channels = sim_rx_channels_;
            b = make_sim_backend(opts);
        }
        b->apply(settings_);
        rx_gain_actual_ = b->rx_gain();
        rx_channel_count_ = b->rx_channel_count();
        if (b->rx_channel_count() > 1) rx2_gain_actual_ = b->rx_gain(1);
        device_info_ = b->describe();
    } catch (const DeviceError& e) {
        if (connected_ || !alarms().active(kDeviceAlarm)) logger()->error("radio: {}", e.what());
        connected_ = false;
        alarms().raise(kDeviceAlarm, forge::Severity::error, std::string("Radio not available: ") + e.what());
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
    ++tuning_version_;
    logger()->info("radio: connected to {} (RX {:.3f} MHz, {:.3f} MSPS, {} RX channel{})", backend_->name(),
                   settings_.rx_lo_hz / 1e6, settings_.sample_rate_hz / 1e6, rx_channel_count_.load(),
                   rx_channel_count_ > 1 ? "s" : "");
    if (rx2_enabled_ && rx_channel_count_ < 2) {
        alarms().raise(kRx2Alarm, forge::Severity::warning,
                       "RX2 is enabled but the transceiver has one RX channel (AD9364): streaming RX1 only");
    } else {
        alarms().clear(kRx2Alarm);
    }
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
    close_backend();  // the 5 s timer reconnects
}

// ---------------------------------------------------------------------------
// RX thread: device -> ZeroMQ PUB

void RadioController::rx_loop(std::promise<Endpoints> bound) {
    std::unique_ptr<forge::zmq::Sender> pub, pub2;
    try {
        forge::zmq::SocketOptions so;
        so.send_hwm = 32;  // frames queued per subscriber before that subscriber drops
        pub = std::make_unique<forge::zmq::Sender>(forge::zmq::Sender::Kind::pub,
                                                    config().get_or<std::string>("zmq.rx", "tcp://*:5555"), true, so);
        so.send_hwm = 4;   // RX2 blocks: a slow reader only ever wants the newest
        pub2 = std::make_unique<forge::zmq::Sender>(forge::zmq::Sender::Kind::pub,
                                                     config().get_or<std::string>("zmq.rx2", "tcp://*:5558"), true, so);
        bound.set_value({pub->endpoint(), pub2->endpoint()});
    } catch (...) {
        bound.set_exception(std::current_exception());
        return;
    }

    std::vector<std::int16_t> buf, buf2;
    std::uint64_t seq = 0;
    std::uint64_t generation_seen = ~0ull;
    bool streaming = false;
    unsigned channels = 1;
    auto next_snapshot = std::chrono::steady_clock::now();

    // RX2 blocks: the start of one DMA buffer (contiguous even when samples
    // are lost between buffers), at most rx2_tap_hz_ blocks per second.
    std::uint64_t block_seq = 0;
    std::uint64_t tuning_seen = ~0ull;
    int settle = 0;
    auto next_block = std::chrono::steady_clock::now();

    while (running_) {
        subscriptions_ = pub->subscriptions();
        std::shared_lock lock(backend_mutex_);
        Backend* b = backend_.get();

        if (b && (generation_ != generation_seen || restart_rx_.exchange(false))) {
            if (streaming && generation_ == generation_seen) b->stop_rx();
            streaming = false;
            generation_seen = generation_;
        }
        if (!b || !rx_enabled_) {
            if (streaming && b) b->stop_rx();
            streaming = false;
            lock.unlock();
            std::this_thread::sleep_for(50ms);
            continue;
        }

        const std::size_t size = buffer_size_;
        std::size_t n = 0;
        bool want_rx2 = false;
        try {
            if (!streaming) {
                channels = rx2_enabled_ && b->rx_channel_count() > 1 ? 2 : 1;
                buf.resize(2 * size);
                buf2.resize(channels > 1 ? 2 * size : 0);
                b->start_rx(size, channels);
                streaming = true;
            }
            // RX2 is only converted when a block is due and someone listens.
            want_rx2 = channels > 1 && rx2_subscriptions_ > 0 && settle == 0 &&
                       std::chrono::steady_clock::now() >= next_block;
            n = b->read_rx(buf.data(), want_rx2 ? buf2.data() : nullptr, size);
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

        ++rx_frames_;
        rx_samples_ += n;
        ++seq;

        if (subscriptions_ > 0) {
            const json header{{"seq", seq},
                              {"timestamp_ns", now_ns()},
                              {"samples", n},
                              {"format", "cs16"},
                              {"sample_rate", rate_hz_.load()},
                              {"center_freq", rx_lo_hz_.load()}};
            pub->send("iq", header, buf.data(), n * 2 * sizeof(std::int16_t));
        }

        const auto now = std::chrono::steady_clock::now();
        rx2_subscriptions_ = channels > 1 ? pub2->subscriptions() : 0;
        if (channels > 1 && rx2_subscriptions_ > 0) {
            // Buffers queued before a retune carry the old LO: skip a few.
            if (const auto tv = tuning_version_.load(); tv != tuning_seen) {
                tuning_seen = tv;
                settle = kRx2SettleBuffers;
                want_rx2 = false;
            }
            if (settle > 0) {
                --settle;
            } else if (want_rx2 && n > 0) {
                const std::size_t samples = std::min(n, rx2_tap_samples_);
                const json header{{"seq", ++block_seq},
                                  {"timestamp_ns", now_ns()},
                                  {"samples", samples},
                                  {"format", "cs16"},
                                  {"channel", 2},
                                  {"sample_rate", rate_hz_.load()},
                                  {"center_freq", rx_lo_hz_.load()},
                                  {"rx_gain_db", rx2_gain_actual_.load()}};
                pub2->send("iq", header, buf2.data(), samples * 2 * sizeof(std::int16_t));
                ++rx2_blocks_;
                if (rx2_tap_hz_ > 0) {
                    next_block = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                           std::chrono::duration<double>(1.0 / rx2_tap_hz_));
                }
            }
        }
        if (now >= next_snapshot && n >= spectrum_size_) {
            std::lock_guard l(snapshot_mutex_);
            snapshot_.assign(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(2 * spectrum_size_));
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
    if (m.header.value("format", std::string("cs16")) != "cs16" || m.payload.size() % 4 != 0 || m.payload.empty()) {
        ++tx_dropped_;
        logger()->warn("radio: TX frame ignored (need format cs16 and a payload of int16 I/Q pairs)");
        return;
    }
    const auto* samples = reinterpret_cast<const std::int16_t*>(m.payload.data());
    const std::size_t n = m.payload.size() / 4;
    const bool cyclic = m.header.value("cyclic", false);

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
            b->start_tx(n, true);
            b->write_tx(samples, n);
            tx_mode_ = TxMode::cyclic;
        } else {
            const std::size_t size = buffer_size_;
            if (tx_mode_ != TxMode::streaming) {
                b->start_tx(size, false);
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
            rx_gain_actual_ = backend_->rx_gain();
            if (rx_channel_count_ > 1) rx2_gain_actual_ = backend_->rx_gain(1);
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
    j["rx2_enabled"] = rx2_enabled_.load();
    try {
        forge::save_json(state_file_, j);
    } catch (const std::exception& e) {
        logger()->warn("cannot save {}: {}", state_file_, e.what());
    }
}

}  // namespace radio
