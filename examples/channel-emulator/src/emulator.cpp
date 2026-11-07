#include "emulator.hpp"

#include "radio/v1/radio.pb.h"

#include <cmath>
#include <sstream>

using namespace std::chrono_literals;

namespace emulator {
namespace {

constexpr const char* kClipAlarm = "emulator.clipping";
constexpr const char* kCpuAlarm = "emulator.cpu";

double round_to(double v, double step) { return std::round(v / step) * step; }

}  // namespace

std::vector<channel::Path> parse_paths(const std::string& text) {
    std::vector<channel::Path> out;
    std::stringstream all(text);
    std::string item;
    while (std::getline(all, item, ',')) {
        if (item.find_first_not_of(" \t") == std::string::npos) continue;
        std::stringstream one(item);
        std::string a, b, c;
        std::getline(one, a, ':');
        std::getline(one, b, ':');
        std::getline(one, c, ':');
        try {
            channel::Path p;
            p.delay_us = std::stod(a);
            p.gain_db = b.empty() ? 0 : std::stod(b);
            p.phase_deg = c.empty() ? 0 : std::stod(c);
            if (p.delay_us < 0 || p.delay_us > 50) throw std::invalid_argument("delay");
            out.push_back(p);
        } catch (const std::exception&) {
            throw std::invalid_argument("path '" + item + "': expected delay_us:gain_db[:phase_deg], delay 0..50 us");
        }
    }
    if (out.size() > 16) throw std::invalid_argument("at most 16 paths");
    return out;
}

std::string format_paths(const std::vector<channel::Path>& paths) {
    std::string s;
    for (const auto& p : paths) {
        if (!s.empty()) s += ", ";
        s += fmt::format("{:g}:{:g}:{:g}", p.delay_us, p.gain_db, p.phase_deg);
    }
    return s;
}

ChannelEmulator::ChannelEmulator()
    : Application({"channel-emulator", EMULATOR_VERSION, "Radio channel impairments in the TX path of radio-controller"}) {}

std::string ChannelEmulator::input_endpoint() const { return in_ ? in_->endpoint() : ""; }

// ---------------------------------------------------------------------------
// Configuration and controls

void ChannelEmulator::setup_config(forge::config::Schema& s) {
    s.section("emulator", "Channel emulator");
    s.string("emulator.input", "tcp://*:5586", "TX blocks come in here (PULL; the modem's radio_tx points here)");
    s.string("emulator.output", "tcp://127.0.0.1:5556", "and go on to radio-controller [zmq].tx (PUSH)");
    s.string("emulator.radio_control", "tcp://127.0.0.1:5557",
             "radio-controller [forge].control, read for the sample rate (\"\" = use sample_rate_hz)");
    s.number("emulator.sample_rate_hz", 3e6, "Sample rate when radio-controller cannot tell").range(1e3, 122.88e6).unit("Hz");
    std::string names;
    for (const auto& n : channel::preset_names()) names += (names.empty() ? "" : ", ") + n;
    s.string("emulator.preset", "clean", "Settings at start: " + names);
    s.number("emulator.noise_bandwidth_hz", 750e3,
             "Bandwidth the SNR is defined in: the modem's symbol rate makes it Es/N0 (0 = whole sample rate)")
        .range(0, 122.88e6)
        .unit("Hz");
    s.integer("emulator.seed", 1, "Random seed (noise, fading)").range(0, 1 << 30);
}

template <class T>
void ChannelEmulator::bind(forge::web::Group& g, const std::string& id, const std::string& label,
                           T channel::Settings::*field, double min, double max, double step, const std::string& unit) {
    if constexpr (std::is_same_v<T, bool>) {
        g.toggle(id, label, [this, field] { return settings_.*field; },
                 [this, field](bool on) {
                     settings_.*field = on;
                     preset_ = "custom";
                     changed();
                 });
    } else {
        auto c = g.number(id, label, [this, field] { return settings_.*field; },
                          [this, field](double v) {
                              settings_.*field = v;
                              preset_ = "custom";
                              changed();
                          });
        c.range(min, max, step);
        if (!unit.empty()) c.unit(unit);
    }
}

void ChannelEmulator::setup_web(forge::web::Dashboard& ui) {
    using S = channel::Settings;
    auto& e = ui.group("Emulator");
    auto names = channel::preset_names();
    names.push_back("custom");
    e.select("preset", "Preset", names, [this] { return preset_; }, [this](const std::string& p) { apply(p); });
    bind(e, "bypass", "Bypass (no impairments)", &S::bypass);
    e.value("sample_rate", "Sample rate", [this] { return msps_; }).unit("MSPS").precision(3);
    e.value("rate_source", "Rate from", [this] { return rate_source_; });
    e.value("signal_dbfs", "Signal (while present)", [this] { return round_to(signal_dbfs_.load(), 0.1); }).unit("dBFS");
    e.value("blocks", "Blocks", [this] { return blocks_.load(); });
    e.value("clipped", "Clipped samples", [this] { return clipped_.load(); });
    e.value("cpu_percent", "CPU (of real time)", [this] { return round_to(cpu_.load() * 100, 0.1); }).unit("%");
    e.value("waits", "Waits for radio-controller", [this] { return waits_.load(); });
    e.value("cyclic", "Cyclic frames (passed as they are)", [this] { return cyclic_.load(); });
    e.button("reset_stats", "Reset statistics", [this] {
        blocks_ = samples_ = clipped_ = cyclic_ = dropped_ = waits_ = 0;
        last_clipped_ = last_samples_ = 0;
    });

    auto& level = ui.group("Level and noise");
    bind(level, "gain_db", "Path gain", &S::gain_db, -60, 20, 0.5, "dB");
    bind(level, "noise", "Noise", &S::noise);
    bind(level, "snr_db", "SNR", &S::snr_db, -10, 60, 0.5, "dB");
    level.number("noise_bw_khz", "SNR bandwidth", [this] { return noise_bandwidth_hz_ / 1e3; },
                 [this](double v) {
                     noise_bandwidth_hz_ = v * 1e3;
                     settings_.noise_bandwidth_hz = noise_bandwidth_hz_;
                     changed();
                 })
        .range(0, 122880, 1)
        .unit("kHz");

    auto& f = ui.group("Frequency and timing");
    bind(f, "frequency", "Frequency offset", &S::frequency);
    bind(f, "cfo_hz", "Offset", &S::cfo_hz, -1e6, 1e6, 10, "Hz");
    bind(f, "doppler_hz", "Doppler (sine) amplitude", &S::doppler_hz, 0, 1e6, 10, "Hz");
    bind(f, "doppler_period_s", "Doppler period", &S::doppler_period_s, 0.01, 3600, 0.01, "s");
    bind(f, "phase_noise", "Phase noise", &S::phase_noise);
    bind(f, "linewidth_hz", "Oscillator linewidth", &S::linewidth_hz, 0, 1e5, 1, "Hz");
    bind(f, "clock", "Sample clock offset", &S::clock);
    bind(f, "clock_ppm", "Clock error", &S::clock_ppm, -1000, 1000, 0.1, "ppm");
    f.value("timing_resets", "Clock: resets in gaps / forced",
            [this] { return fmt::format("{} / {}", timing_resets_.load(), timing_forced_.load()); });

    auto& mp = ui.group("Multipath and fading");
    bind(mp, "multipath", "Multipath", &S::multipath);
    mp.text("paths", "Paths (delay_us:gain_db:phase_deg, ...)", [this] { return paths_text_; },
            [this](const std::string& t) {
                auto p = parse_paths(t);  // throws: the control reports the error
                settings_.paths = std::move(p);
                paths_text_ = format_paths(settings_.paths);
                preset_ = "custom";
                changed();
            });
    bind(mp, "fading", "Fading", &S::fading);
    bind(mp, "fading_hz", "Fading Doppler", &S::fading_hz, 0.01, 5000, 0.1, "Hz");
    bind(mp, "rician_k_db", "Rician K (-100 = Rayleigh)", &S::rician_k_db, -100, 40, 0.5, "dB");

    auto& fe = ui.group("Front end and interference");
    bind(fe, "iq", "IQ imbalance", &S::iq);
    bind(fe, "iq_gain_db", "IQ gain error", &S::iq_gain_db, -6, 6, 0.1, "dB");
    bind(fe, "iq_phase_deg", "IQ phase error", &S::iq_phase_deg, -30, 30, 0.1, "deg");
    bind(fe, "dc", "DC offset", &S::dc);
    bind(fe, "dc_dbc", "DC level", &S::dc_dbc, -80, 10, 0.5, "dBc");
    bind(fe, "interference", "Interfering tone", &S::interference);
    bind(fe, "interference_hz", "Tone offset", &S::interference_hz, -60e6, 60e6, 1000, "Hz");
    bind(fe, "interference_dbc", "Tone level", &S::interference_dbc, -80, 30, 0.5, "dBc");
    bind(fe, "interference_on_ms", "Pulsed: on", &S::interference_on_ms, 0, 10000, 0.1, "ms");
    bind(fe, "interference_off_ms", "Pulsed: off (0 = continuous)", &S::interference_off_ms, 0, 10000, 0.1, "ms");
}

// ---------------------------------------------------------------------------
// Start, stop

void ChannelEmulator::on_start() {
    const auto cfg = config();
    noise_bandwidth_hz_ = cfg.value<double>("emulator.noise_bandwidth_hz");
    radio_control_ = cfg.value<std::string>("emulator.radio_control");
    rate_hz_ = cfg.value<double>("emulator.sample_rate_hz");
    msps_ = rate_hz_ / 1e6;
    const auto preset_name = cfg.value<std::string>("emulator.preset");
    try {
        channel::preset(preset_name);
    } catch (const std::invalid_argument& e) {
        throw forge::ConfigError(std::string("emulator.preset: ") + e.what());
    }
    channel_ = std::make_unique<channel::Channel>(rate_hz_.load(), cfg.value<std::uint64_t>("emulator.seed"));
    apply(preset_name);

    forge::zmq::SocketOptions so;
    so.send_hwm = 4;  // backpressure from radio-controller, not a queue here
    out_ = std::make_unique<forge::zmq::Sender>(forge::zmq::Sender::Kind::push, cfg.value<std::string>("emulator.output"),
                                                false, so);
    forge::zmq::Receiver::Options ro;
    ro.bind = true;
    ro.socket.recv_hwm = 4;
    running_ = true;
    in_ = std::make_unique<forge::zmq::Receiver>(forge::zmq::Receiver::Kind::pull, cfg.value<std::string>("emulator.input"),
                                                 [this](forge::zmq::Message&& m) { on_block(std::move(m)); }, ro);
    logger()->info("emulator: {} -> {} (preset {}), sample rate from {}", in_->endpoint(),
                   cfg.value<std::string>("emulator.output"), preset_name,
                   radio_control_.empty() ? "the configuration" : radio_control_);
    if (!radio_control_.empty()) {
        read_sample_rate();
        every(5s, [this] { read_sample_rate(); });
    }
    last_tick_ = std::chrono::steady_clock::now();
    every(1s, [this] { tick(); });
}

void ChannelEmulator::on_stop() {
    running_ = false;
    in_.reset();  // joins the receiver thread (it gives up waiting for the radio when running_ is false)
    out_.reset();
}

// ---------------------------------------------------------------------------
// Settings (main loop)

void ChannelEmulator::apply(const std::string& name) {
    if (name != "custom") {
        const bool bypass = settings_.bypass;
        settings_ = channel::preset(name);  // throws std::invalid_argument for an unknown name
        settings_.bypass = bypass;
        settings_.noise_bandwidth_hz = noise_bandwidth_hz_;
        paths_text_ = format_paths(settings_.paths);
        if (paths_text_.empty()) paths_text_ = "0:0:0, 0.5:-3:90, 1.3:-8:200";  // a start for editing
        if (settings_.paths.empty()) settings_.paths = parse_paths(paths_text_);
    }
    preset_ = name;
    changed();
}

void ChannelEmulator::changed() {
    {
        std::lock_guard lock(pending_mutex_);
        pending_ = settings_;
    }
    ++version_;
}

void ChannelEmulator::read_sample_rate() {
    if (reading_rate_) return;
    reading_rate_ = true;
    const auto endpoint = radio_control_;
    background(
        [endpoint]() -> double {
            try {
                forge::zmq::Client c(endpoint, 2s);
                c.set_client("channel-emulator");
                return c.get("sample_rate_msps").get<double>() * 1e6;
            } catch (const std::exception&) {
                return 0;
            }
        },
        [this](double hz) {
            reading_rate_ = false;
            if (hz > 0) {
                rate_hz_ = hz;
                rate_source_ = "radio-controller";
            } else {
                rate_source_ = "configuration (radio-controller not answering)";
            }
            msps_ = rate_hz_ / 1e6;
        });
}

void ChannelEmulator::tick() {
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - last_tick_).count();
    last_tick_ = now;
    const auto clipped = clipped_.load(), samples = samples_.load();
    const bool clipping = clipped > last_clipped_ + (samples - last_samples_) / 1000;  // > 0.1 %
    alarms().set(kClipAlarm, clipping, forge::Severity::warning,
                 "output clipped at full scale: lower the gain, the interference/DC level or the sender's TX level");
    last_clipped_ = clipped;
    last_samples_ = samples;
    const bool busy = cpu_ > 0.9 && dt > 0 && samples > 0;
    alarms().set(kCpuAlarm, busy, forge::Severity::warning,
                 fmt::format("processing takes {:.0f} % of real time: turn impairments off or lower the sample rate",
                             cpu_ * 100));
}

// ---------------------------------------------------------------------------
// Receiver thread: impair and forward every block

void ChannelEmulator::on_block(forge::zmq::Message&& m) {
    radio::v1::TxBlock tx;
    try {
        tx = m.as<radio::v1::TxBlock>();
    } catch (const forge::zmq::Error&) {
        ++dropped_;
        return;
    }
    if ((!tx.format().empty() && tx.format() != "cs16") || m.blob.size() % 4 != 0 || m.blob.empty()) {
        ++dropped_;
        return;
    }
    const std::size_t n = m.blob.size() / 4;
    if (tx.cyclic()) {
        ++cyclic_;  // a repeated test frame: impairments would repeat with it, pass it as it is
    } else {
        if (version_ != applied_version_) {
            std::lock_guard lock(pending_mutex_);
            applied_version_ = version_;
            channel_->configure(pending_);
        }
        channel_->set_sample_rate(rate_hz_);
        const auto t0 = std::chrono::steady_clock::now();
        channel_->process(reinterpret_cast<std::int16_t*>(m.blob.data()), n);
        const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double real = static_cast<double>(n) / channel_->sample_rate();
        cpu_ = 0.9 * cpu_ + 0.1 * (spent / real);
        const auto st = channel_->stats();
        clipped_ = st.clipped;
        timing_resets_ = st.timing_resets;
        timing_forced_ = st.timing_forced;
        signal_dbfs_ = st.signal_dbfs;
    }
    // Forward; wait while radio-controller is a few blocks behind (backpressure to the sender).
    zmq::pollitem_t item{static_cast<void*>(out_->native()), 0, ZMQ_POLLOUT, 0};
    while (running_) {
        if (zmq::poll(&item, 1, std::chrono::milliseconds(50)) > 0 && (item.revents & ZMQ_POLLOUT) &&
            out_->publish("tx", tx, m.blob.data(), m.blob.size())) {
            ++blocks_;
            samples_ += n;
            return;
        }
        ++waits_;
    }
}

}  // namespace emulator
