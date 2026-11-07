#include "analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

#ifndef SA_VERSION
#define SA_VERSION "0.0.0"
#endif

using namespace std::chrono_literals;
using forge::json;
using forge::http::HttpError;

namespace sa {
namespace {

constexpr double kMinFreq = 45e6;    // LO range (70 MHz - 6 GHz) +- part of the capture
constexpr double kMaxFreq = 6025e6;
constexpr const char* kDeviceAlarm = "analyzer.device";
constexpr const char* kOverloadAlarm = "analyzer.overload";
constexpr const char* kLimitAlarm = "analyzer.limit";

struct Preset {
    const char* label;
    double center_mhz;
    double span_mhz;
};
const std::map<std::string, Preset>& presets() {
    static const std::map<std::string, Preset> p{
        {"ism_433", {"ISM 433 MHz", 433.92, 2}},
        {"ism_868", {"SRD 868 MHz", 868.0, 4}},
        {"ism_915", {"ISM 902-928 MHz", 915.0, 26}},
        {"gps_l1", {"GPS L1", 1575.42, 5}},
        {"wifi_2g4", {"Wi-Fi 2.4 GHz", 2441.75, 90}},
        {"wifi_5g", {"Wi-Fi 5 GHz", 5500.0, 700}},
        {"full", {"Full span", 3035.0, 5930}},
    };
    return p;
}

std::vector<std::string> preset_ids() {
    std::vector<std::string> ids{"custom"};
    for (const auto& [id, p] : presets()) ids.push_back(id);
    return ids;
}

/// MHz for the number inputs, rounded to 1 Hz (no 2439.9999999 in the UI).
double mhz(double hz) { return std::round(hz) / 1e6; }

int tenths(double v) { return static_cast<int>(std::lround(std::clamp(v, -250.0, 100.0) * 10)); }

json tenths_array(const std::vector<double>& v) {
    json a = json::array();
    for (double x : v) a.push_back(tenths(x));
    return a;
}

std::string fmt_freq(double hz) {
    if (std::abs(hz) >= 1e9) return fmt::format("{:.6f} GHz", hz / 1e9);
    if (std::abs(hz) >= 1e6) return fmt::format("{:.4f} MHz", hz / 1e6);
    return fmt::format("{:.2f} kHz", hz / 1e3);
}

json settings_json(const Settings& s, double ref, double scale, bool limit_on, double limit) {
    json m = json::array();
    for (const auto& mk : s.markers) m.push_back({{"enabled", mk.enabled}, {"freq_hz", mk.freq_hz}, {"delta", mk.delta}});
    return {{"center_hz", s.center_hz},     {"span_hz", s.span_hz},        {"rbw_hz", s.rbw_hz},
            {"vbw_averages", s.vbw_averages}, {"window", s.window},        {"gain_mode", s.gain_mode},
            {"rx_gain_db", s.rx_gain_db},   {"unit", s.unit},              {"cal_offset_db", s.cal_offset_db},
            {"trace_modes", s.trace_modes}, {"trace_average", s.trace_average}, {"detector", s.detector},
            {"points", s.points},           {"markers", m},                {"active_marker", s.active_marker},
            {"peak_track", s.peak_track},   {"measurement", s.measurement}, {"channel_bw_hz", s.channel_bw_hz},
            {"obw_percent", s.obw_percent}, {"update_hz", s.update_hz},   {"ref_level", ref},
            {"scale_db", scale},            {"limit_on", limit_on},        {"limit_level", limit}};
}

}  // namespace

Analyzer::Analyzer() : Application({"spectrum-analyzer", SA_VERSION, "AD9364 spectrum analyzer"}) {}

void Analyzer::setup_cli(CLI::App& cli) {
    cli.add_option("--backend", cli_backend_, "Radio backend: sim | iio (overrides [analyzer].backend)")
        ->check(CLI::IsMember({"sim", "iio"}));
    cli.add_option("--uri", cli_uri_, "libiio context URI, e.g. ip:192.168.2.1, local:");
}

// ---------------------------------------------------------------------------
// configuration and persistence

void Analyzer::load(const forge::Config& cfg) {
    backend_kind_ = cli_backend_.empty() ? cfg.get_or<std::string>("analyzer.backend", "sim") : cli_backend_;
    uri_ = cli_uri_.empty() ? cfg.get_or<std::string>("analyzer.uri", "") : cli_uri_;
    timeout_ms_ = cfg.get_or<unsigned>("analyzer.timeout_ms", 2000);
    state_file_ = cfg.get_or<std::string>("analyzer.state_file", "");
    limits_.sweep_rate_hz = std::clamp(cfg.get_or<double>("analyzer.sweep_rate_hz", 61.44e6), 2.083334e6, 61.44e6);
    limits_.usable_fraction = std::clamp(cfg.get_or<double>("analyzer.usable_fraction", 0.8), 0.3, 0.95);

    json j = cfg.section("analyzer").to_json();
    if (!state_file_.empty() && std::filesystem::exists(state_file_)) {
        try {
            j.update(forge::load_json(state_file_));
        } catch (const std::exception& e) {
            logger()->warn("ignoring {}: {}", state_file_, e.what());
        }
    }
    Settings s;
    s.center_hz = std::clamp(j.value("center_hz", s.center_hz), kMinFreq, kMaxFreq);
    s.span_hz = std::clamp(j.value("span_hz", s.span_hz), 10e3, kMaxFreq - kMinFreq);
    s.rbw_hz = std::max(0.0, j.value("rbw_hz", s.rbw_hz));
    s.vbw_averages = std::clamp<std::size_t>(j.value("vbw_averages", s.vbw_averages), 1, 64);
    s.window = j.value("window", s.window);
    s.gain_mode = j.value("gain_mode", s.gain_mode);
    s.rx_gain_db = std::clamp(j.value("rx_gain_db", s.rx_gain_db), -3.0, 71.0);
    s.unit = j.value("unit", s.unit);
    s.cal_offset_db = j.value("cal_offset_db", s.cal_offset_db);
    if (j.contains("trace_modes") && j["trace_modes"].is_array()) {
        for (std::size_t i = 0; i < kTraces && i < j["trace_modes"].size(); ++i) {
            s.trace_modes[i] = j["trace_modes"][i].get<std::string>();
        }
    }
    s.trace_average = std::clamp<std::size_t>(j.value("trace_average", s.trace_average), 2, 1000);
    s.detector = j.value("detector", s.detector);
    s.points = std::clamp<std::size_t>(j.value("points", s.points), 101, 4001);
    if (j.contains("markers") && j["markers"].is_array()) {
        for (std::size_t i = 0; i < kMarkers && i < j["markers"].size(); ++i) {
            const auto& m = j["markers"][i];
            s.markers[i] = {m.value("enabled", false), m.value("freq_hz", s.center_hz), m.value("delta", false)};
        }
    }
    for (auto& m : s.markers) {
        if (m.freq_hz <= 0) m.freq_hz = s.center_hz;
    }
    s.active_marker = std::min<std::size_t>(j.value("active_marker", std::size_t{0}), kMarkers - 1);
    s.peak_track = j.value("peak_track", false);
    s.measurement = j.value("measurement", s.measurement);
    s.channel_bw_hz = std::max(1e3, j.value("channel_bw_hz", s.channel_bw_hz));
    s.obw_percent = std::clamp(j.value("obw_percent", s.obw_percent), 50.0, 99.99);
    s.update_hz = std::clamp(j.value("update_hz", s.update_hz), 1.0, 30.0);
    s.settle_buffers = j.value("settle_buffers", backend_kind_ == "iio" ? std::size_t{3} : std::size_t{1});
    // validate names early: a bad config should fail at start-up, not in the engine
    radio::dsp::parse_window(s.window);
    parse_detector(s.detector);
    for (const auto& m : s.trace_modes) parse_trace_mode(m);
    settings_ = s;

    ref_level_ = j.value("ref_level", s.unit == "dBm" ? -20.0 : 0.0);
    scale_db_ = j.value("scale_db", 10.0);
    limit_on_ = j.value("limit_on", false);
    limit_level_ = j.value("limit_level", -40.0);
}

void Analyzer::changed() {
    if (engine_) engine_->update(settings_);
    dirty_ = true;
}

void Analyzer::save_state() {
    if (!dirty_ || state_file_.empty()) return;
    dirty_ = false;
    try {
        forge::save_json(state_file_, settings_json(settings_, ref_level_, scale_db_, limit_on_, limit_level_));
    } catch (const std::exception& e) {
        logger()->warn("cannot save {}: {}", state_file_, e.what());
    }
}

void Analyzer::set_frequency(double center, double span) {
    span = std::round(std::clamp(span, 10e3, kMaxFreq - kMinFreq));
    center = std::round(std::clamp(center, kMinFreq + span / 2, kMaxFreq - span / 2));
    settings_.center_hz = center;
    settings_.span_hz = span;
    changed();
}

// ---------------------------------------------------------------------------
// controls (web panel, analyzer page, console and ZeroMQ all use these)

void Analyzer::setup_web(forge::web::Dashboard& ui) {
    load(config());
    ui.title("Spectrum analyzer");
    ui.link("Analyzer", "/");
    auto& s = settings_;

    // --- frequency ---------------------------------------------------------------
    auto& fq = ui.group("Frequency");
    fq.number("center_mhz", "Center", [this] { return mhz(settings_.center_hz); },
              [this](double v) { set_frequency(v * 1e6, settings_.span_hz); })
        .range(kMinFreq / 1e6, kMaxFreq / 1e6, 0.000001)
        .unit("MHz")
        .precision(4);
    fq.number("span_mhz", "Span", [this] { return mhz(settings_.span_hz); },
              [this](double v) { set_frequency(settings_.center_hz, v * 1e6); })
        .range(0.01, (kMaxFreq - kMinFreq) / 1e6, 0.000001)
        .unit("MHz")
        .precision(4);
    fq.number("start_mhz", "Start", [this] { return mhz(settings_.center_hz - settings_.span_hz / 2); },
              [this](double v) {
                  const double stop = settings_.center_hz + settings_.span_hz / 2;
                  if (v * 1e6 >= stop) throw HttpError(400, "start must be below stop");
                  set_frequency((v * 1e6 + stop) / 2, stop - v * 1e6);
              })
        .range(kMinFreq / 1e6, kMaxFreq / 1e6, 0.000001)
        .unit("MHz")
        .precision(4);
    fq.number("stop_mhz", "Stop", [this] { return mhz(settings_.center_hz + settings_.span_hz / 2); },
              [this](double v) {
                  const double start = settings_.center_hz - settings_.span_hz / 2;
                  if (v * 1e6 <= start) throw HttpError(400, "stop must be above start");
                  set_frequency((start + v * 1e6) / 2, v * 1e6 - start);
              })
        .range(kMinFreq / 1e6, kMaxFreq / 1e6, 0.000001)
        .unit("MHz")
        .precision(4);
    fq.select("preset", "Preset", preset_ids(),
              [this] {
                  for (const auto& [id, p] : presets()) {
                      if (std::abs(settings_.center_hz - p.center_mhz * 1e6) < 1 &&
                          std::abs(settings_.span_hz - p.span_mhz * 1e6) < 1) {
                          return id;
                      }
                  }
                  return std::string("custom");
              },
              [this](const std::string& id) {
                  if (id == "custom") return;
                  const auto& p = presets().at(id);
                  set_frequency(p.center_mhz * 1e6, p.span_mhz * 1e6);
              })
        .help("ISM 433/868/915, GPS L1, Wi-Fi 2.4/5 GHz, full span");
    fq.button("full_span", "Full span", [this] { set_frequency(3035e6, 5930e6); });
    fq.value("sweep_mode", "Mode", [this] {
        const auto f = engine_ ? engine_->latest() : nullptr;
        if (!f) return std::string("-");
        return f->plan.swept ? fmt::format("swept, {} segments", f->plan.segments.size()) : std::string("realtime");
    });

    // --- bandwidth ------------------------------------------------------------------
    auto& bw = ui.group("Bandwidth");
    bw.toggle("rbw_auto", "Auto RBW", [this] { return settings_.rbw_hz <= 0; },
              [this](bool on) {
                  if (on) settings_.rbw_hz = 0;
                  else if (auto f = latest()) settings_.rbw_hz = f->plan.rbw_hz;
                  else settings_.rbw_hz = 100e3;
                  changed();
              });
    bw.number("rbw_khz", "RBW", [this] {
                  const auto f = latest();
                  const double hz = settings_.rbw_hz > 0 ? settings_.rbw_hz : f ? f->plan.rbw_hz : 0.0;
                  return std::round(hz) / 1e3;
              },
              [this](double khz) {
                  settings_.rbw_hz = khz * 1e3;
                  changed();
              })
        .range(0.1, 10000, 0.001)
        .unit("kHz")
        .precision(3)
        .help("Set a value to switch to manual RBW; the actual RBW depends on the FFT size");
    bw.number("vbw_averages", "Video averaging", [this] { return double(settings_.vbw_averages); },
              [this](double v) {
                  settings_.vbw_averages = static_cast<std::size_t>(v);
                  changed();
              })
        .range(1, 64, 1)
        .unit("FFTs")
        .help("FFTs averaged per capture: smoother noise, slower sweep");
    bw.select("window", "Window", radio::dsp::window_names(), [this] { return settings_.window; },
              [this](const std::string& w) {
                  settings_.window = w;
                  changed();
              });
    bw.value("rbw_actual", "Actual RBW", [this] {
        const auto f = latest();
        return f ? fmt_freq(f->plan.rbw_hz) : std::string("-");
    });
    bw.value("fft_size", "FFT size", [this] {
        const auto f = latest();
        return f ? json(f->plan.fft_size) : json(nullptr);
    });
    bw.value("sample_rate", "Sample rate", [this] {
        const auto f = latest();
        return f ? fmt::format("{:.3f} MSPS", f->plan.sample_rate_hz / 1e6) : std::string("-");
    });

    // --- amplitude -----------------------------------------------------------------
    auto& am = ui.group("Amplitude");
    am.number("ref_level", "Reference level", ref_level_).range(-150, 30, 0.1).unit("dB").precision(1);
    am.select("scale_db", "Scale", {"1", "2", "5", "10", "20"},
              [this] { return fmt::format("{}", static_cast<int>(scale_db_)); },
              [this](const std::string& v) {
                  scale_db_ = std::stod(v);
                  dirty_ = true;
              })
        .help("dB per division (10 divisions)");
    am.select("unit", "Unit", {"dBFS", "dBm"}, [this] { return settings_.unit; },
              [this](const std::string& u) {
                  settings_.unit = u;
                  changed();
              });
    am.number("cal_offset_db", "dBm calibration", [this] { return settings_.cal_offset_db; },
              [this](double v) {
                  settings_.cal_offset_db = v;
                  changed();
              })
        .range(-100, 100, 0.1)
        .unit("dB")
        .help("dBm = dBFS - RX gain + calibration (measure a known signal to find it)");
    am.select("gain_mode", "Gain control", radio::gain_modes(), [this] { return settings_.gain_mode; },
              [this](const std::string& m) {
                  settings_.gain_mode = m;
                  changed();
              })
        .help("Manual gain keeps levels comparable between sweeps");
    am.slider("rx_gain_db", "RX gain", [this] { return settings_.rx_gain_db; },
              [this](double v) {
                  if (settings_.gain_mode != "manual") throw HttpError(409, "set gain control to manual first");
                  settings_.rx_gain_db = v;
                  changed();
              },
              -3, 71, 1)
        .unit("dB")
        .precision(0);
    am.value("overload", "ADC overload", [this] {
        const auto f = latest();
        return f && f->overload;
    });

    // --- traces --------------------------------------------------------------------
    auto& tr = ui.group("Traces");
    for (std::size_t i = 0; i < kTraces; ++i) {
        tr.select(fmt::format("trace{}_mode", i + 1), fmt::format("Trace {}", i + 1), trace_mode_names(),
                  [this, i] { return settings_.trace_modes[i]; },
                  [this, i](const std::string& m) {
                      settings_.trace_modes[i] = m;
                      changed();
                  });
    }
    tr.number("trace_average", "Average count", [this] { return double(settings_.trace_average); },
              [this](double v) {
                  settings_.trace_average = static_cast<std::size_t>(v);
                  changed();
              })
        .range(2, 1000, 1)
        .unit("sweeps");
    tr.select("detector", "Detector", detector_names(), [this] { return settings_.detector; },
              [this](const std::string& d) {
                  settings_.detector = d;
                  changed();
              });
    tr.select("points", "Display points", {"401", "801", "1001", "2001", "4001"},
              [this] { return std::to_string(settings_.points); },
              [this](const std::string& p) {
                  settings_.points = std::stoul(p);
                  changed();
              });
    tr.button("reset_traces", "Restart traces", [this] {
        if (engine_) engine_->reset_traces();
    });

    // --- markers -------------------------------------------------------------------
    auto& mk = ui.group("Markers");
    mk.select("active_marker", "Active marker", {"1", "2", "3", "4"},
              [this] { return std::to_string(settings_.active_marker + 1); },
              [this](const std::string& v) {
                  settings_.active_marker = std::stoul(v) - 1;
                  changed();
              });
    mk.select("marker_trace", "Marker trace", {"1", "2", "3"},
              [this] { return std::to_string(settings_.marker_trace + 1); },
              [this](const std::string& v) {
                  settings_.marker_trace = std::stoul(v) - 1;
                  changed();
              });
    for (std::size_t i = 0; i < kMarkers; ++i) {
        mk.toggle(fmt::format("m{}_on", i + 1), fmt::format("Marker {}", i + 1),
                  [this, i] { return settings_.markers[i].enabled; },
                  [this, i](bool on) {
                      auto& m = settings_.markers[i];
                      if (on && !m.enabled) {
                          // start on the strongest peak no other marker sits on
                          m.freq_hz = settings_.center_hz;
                          if (const auto f = latest()) {
                              const double tol = std::max(f->plan.rbw_hz, (f->stop_hz - f->start_hz) / double(f->points));
                              for (const auto& p : f->peaks) {
                                  const bool taken = std::any_of(settings_.markers.begin(), settings_.markers.end(),
                                      [&](const MarkerSettings& o) { return o.enabled && std::abs(o.freq_hz - p.freq_hz) <= tol; });
                                  if (!taken) {
                                      m.freq_hz = p.freq_hz;
                                      break;
                                  }
                              }
                          }
                          settings_.active_marker = i;
                      }
                      m.enabled = on;
                      changed();
                  });
        mk.number(fmt::format("m{}_mhz", i + 1), fmt::format("Marker {} frequency", i + 1),
                  [this, i] { return mhz(settings_.markers[i].freq_hz); },
                  [this, i](double v) {
                      auto& m = settings_.markers[i];
                      m.freq_hz = v * 1e6;
                      m.enabled = true;
                      settings_.active_marker = i;
                      changed();
                  })
            .range(kMinFreq / 1e6, kMaxFreq / 1e6, 0.000001)
            .unit("MHz")
            .precision(4);
        if (i > 0) {
            mk.toggle(fmt::format("m{}_delta", i + 1), fmt::format("Marker {} delta", i + 1),
                      [this, i] { return settings_.markers[i].delta; },
                      [this, i](bool on) {
                          settings_.markers[i].delta = on;
                          changed();
                      })
                .help("Show frequency and level relative to marker 1");
        }
    }
    mk.button("peak_search", "Peak search", [this] { peak_search(); }).primary();
    mk.button("next_peak", "Next peak", [this] { next_peak(0); });
    mk.button("peak_left", "Peak left", [this] { next_peak(-1); });
    mk.button("peak_right", "Peak right", [this] { next_peak(+1); });
    mk.button("marker_to_center", "Marker to center", [this] {
        const auto& m = active();
        if (!m.enabled) throw HttpError(409, "the active marker is off");
        set_frequency(m.freq_hz, settings_.span_hz);
    });
    mk.toggle("peak_track", "Peak track", [this] { return settings_.peak_track; },
              [this](bool on) {
                  settings_.peak_track = on;
                  if (on) active().enabled = true;
                  changed();
              })
        .help("The active marker follows the strongest signal");
    mk.toggle("signal_track", "Signal track", [this] { return settings_.signal_track; },
              [this](bool on) {
                  settings_.signal_track = on;
                  changed();
              })
        .help("Retune so the strongest signal stays at the center");

    // --- measurements -----------------------------------------------------------------
    auto& ms = ui.group("Measurements");
    ms.select("measurement", "Measurement", {"off", "channel_power", "obw"}, [this] { return settings_.measurement; },
              [this](const std::string& m) {
                  settings_.measurement = m;
                  changed();
              });
    ms.number("channel_bw_mhz", "Channel bandwidth", [this] { return settings_.channel_bw_hz / 1e6; },
              [this](double v) {
                  settings_.channel_bw_hz = v * 1e6;
                  changed();
              })
        .range(0.001, 6000, 0.001)
        .unit("MHz")
        .precision(3);
    ms.number("obw_percent", "OBW power", [this] { return settings_.obw_percent; },
              [this](double v) {
                  settings_.obw_percent = v;
                  changed();
              })
        .range(50, 99.99, 0.01)
        .unit("%");
    ms.value("result", "Result", [this] {
        const auto f = latest();
        if (!f) return std::string("-");
        if (f->channel_power) return fmt::format("{:.2f} {}", *f->channel_power, f->unit);
        if (f->obw) return fmt::format("{} OBW", fmt_freq(f->obw->bandwidth_hz));
        return std::string("-");
    });
    ms.toggle("limit_on", "Limit line", limit_on_).help("Raise an alarm when the live trace crosses the line");
    ms.number("limit_level", "Limit level", limit_level_).range(-200, 50, 0.1).unit("dB").precision(1);
    ms.number("peak_threshold", "Peak table threshold", [this] { return settings_.peak_threshold_db; },
              [this](double v) {
                  settings_.peak_threshold_db = v;
                  changed();
              })
        .range(-200, 50, 1)
        .unit("dB");

    // --- sweep / device ----------------------------------------------------------------
    auto& sw = ui.group("Sweep");
    sw.toggle("run", "Run", [this] { return settings_.run; },
              [this](bool on) {
                  settings_.run = on;
                  changed();
              });
    sw.button("single", "Single sweep", [this] {
        settings_.run = false;
        changed();
        if (engine_) engine_->single();
    });
    sw.number("update_hz", "Max update rate", [this] { return settings_.update_hz; },
              [this](double v) {
                  settings_.update_hz = v;
                  changed();
              })
        .range(1, 30, 1)
        .unit("Hz");
    sw.value("sweep_time", "Sweep time", [this] {
        const auto f = latest();
        return f ? fmt::format("{:.1f} ms", f->sweep_ms) : std::string("-");
    });
    sw.value("connected", "Connected", [this] { return status_.connected; });
    sw.value("backend", "Backend", [this] { return status_.connected ? status_.backend : backend_kind_ + " (not connected)"; });
    sw.value("temperature_c", "Temperature", [this] {
        return status_.temperature_c ? json(*status_.temperature_c) : json(nullptr);
    }).unit("°C").precision(1);
    sw.button("reconnect", "Reconnect", [this] {
        if (engine_) engine_->reconnect();
    });
    (void)s;
}

// ---------------------------------------------------------------------------
// marker commands (main loop)

MarkerSettings& Analyzer::active() { return settings_.markers[std::min(settings_.active_marker, kMarkers - 1)]; }

std::shared_ptr<const Frame> Analyzer::require_frame() const {
    auto f = latest();
    if (!f) throw HttpError(409, "no sweep yet");
    return f;
}

void Analyzer::peak_search() {
    const auto f = require_frame();
    if (f->peaks.empty()) throw HttpError(409, "no peak above the threshold");
    auto& m = active();
    m.enabled = true;
    m.freq_hz = f->peaks.front().freq_hz;
    changed();
}

void Analyzer::next_peak(int direction) {
    const auto f = require_frame();
    auto& m = active();
    if (!m.enabled || f->markers[settings_.active_marker].outside || m.freq_hz < f->start_hz || m.freq_hz > f->stop_hz) {
        return peak_search();
    }
    // Use the marker as set now (the frame may predate the last move) and the
    // level of the marker trace at that frequency.
    const auto& trace = f->traces[std::min(settings_.marker_trace, kTraces - 1)];
    const auto& data = trace.empty() ? f->live : trace;
    if (data.empty()) throw HttpError(409, "no sweep yet");
    const double pos = m.freq_hz;
    const double level = data[point_index(pos, f->start_hz, f->stop_hz, f->points)];
    const double tolerance = std::max(f->plan.rbw_hz, (f->stop_hz - f->start_hz) / double(f->points - 1));
    const PeakPoint* best = nullptr;
    for (const auto& p : f->peaks) {
        if (std::abs(p.freq_hz - pos) <= tolerance) continue;  // the marker's own peak
        if (direction == 0) {
            // next lower peak: the strongest one below the marker level
            if (p.db < level && (!best || p.db > best->db)) best = &p;
        } else {
            const double d = (p.freq_hz - pos) * direction;
            if (d > 0 && (!best || d < (best->freq_hz - pos) * direction)) best = &p;
        }
    }
    if (!best) throw HttpError(409, "no further peak");
    m.freq_hz = best->freq_hz;
    changed();
}

// ---------------------------------------------------------------------------
// HTTP extras and console

json Analyzer::frame_json(const Frame& f) const {
    json traces = json::array();
    for (std::size_t i = 0; i < kTraces; ++i) {
        traces.push_back(f.traces[i].empty() ? json(nullptr)
                                             : json{{"mode", f.trace_modes[i]}, {"data", tenths_array(f.traces[i])}});
    }
    json markers = json::array();
    for (std::size_t i = 0; i < kMarkers; ++i) {
        const auto& m = f.markers[i];
        if (!m.enabled) {
            markers.push_back(nullptr);
            continue;
        }
        if (m.outside) {
            markers.push_back({{"n", i + 1}, {"freq", m.freq_hz}, {"outside", true}});
            continue;
        }
        json j{{"n", i + 1}, {"freq", m.freq_hz}, {"level", std::round(m.level * 100) / 100}};
        if (m.delta) {
            j["dfreq"] = m.dfreq_hz;
            j["dlevel"] = std::round(m.dlevel_db * 100) / 100;
        }
        markers.push_back(std::move(j));
    }
    json peaks = json::array();
    for (const auto& p : f.peaks) peaks.push_back({{"freq", p.freq_hz}, {"level", std::round(p.db * 100) / 100}});
    json j{{"seq", f.seq},
           {"start", f.start_hz},
           {"stop", f.stop_hz},
           {"points", f.points},
           {"unit", f.unit},
           {"ref_level", ref_level_},
           {"scale", scale_db_},
           {"rbw", f.plan.rbw_hz},
           {"fft", f.plan.fft_size},
           {"rate", f.plan.sample_rate_hz},
           {"swept", f.plan.swept},
           {"segments", f.plan.segments.size()},
           {"sweep_ms", std::round(f.sweep_ms * 10) / 10},
           {"overload", f.overload},
           {"gain", f.rx_gain_db},
           {"vbw", settings_.vbw_averages},
           {"detector", settings_.detector},
           {"active_marker", settings_.active_marker + 1},
           {"live", tenths_array(f.live)},
           {"traces", std::move(traces)},
           {"markers", std::move(markers)},
           {"peaks", std::move(peaks)},
           {"limit", {{"on", limit_on_}, {"level", limit_level_}}},
           {"run", settings_.run}};
    if (f.channel_power) {
        j["measurement"] = {{"type", "channel_power"}, {"value", *f.channel_power}, {"bw", settings_.channel_bw_hz},
                            {"center", settings_.center_hz}};
    }
    if (f.obw) {
        j["measurement"] = {{"type", "obw"}, {"bw", f.obw->bandwidth_hz}, {"from", f.obw->from_hz},
                            {"to", f.obw->to_hz}, {"percent", settings_.obw_percent}};
    }
    return j;
}

void Analyzer::setup_http(forge::http::Server& server) {
    // Center and span in one request (zoom from the display): one retune instead of two.
    server.post_json("/api/tune", [this](const forge::http::Request&, const json& body) {
        if (!body.is_object() || !body.contains("center_hz") || !body.contains("span_hz")) {
            throw HttpError(400, "body must be {\"center_hz\": ..., \"span_hz\": ...}");
        }
        const double center = body["center_hz"].get<double>();
        const double span = body["span_hz"].get<double>();
        if (!std::isfinite(center) || !std::isfinite(span) || span <= 0) throw HttpError(400, "invalid frequency");
        return invoke([&] {
            set_frequency(center, span);
            return json{{"center_hz", settings_.center_hz}, {"span_hz", settings_.span_hz}};
        });
    });
    server.get_json("/api/frame", [this](const forge::http::Request&) {
        return invoke([this] {
            const auto f = latest();
            return f ? frame_json(*f) : json(nullptr);
        });
    });
    server.get("/api/trace.csv", [this](const forge::http::Request&, forge::http::Response& res) {
        const auto f = latest();
        if (!f) throw HttpError(409, "no sweep yet");
        std::ostringstream os;
        os << "frequency_hz,live_" << f->unit;
        for (std::size_t t = 0; t < kTraces; ++t) {
            if (!f->traces[t].empty()) os << ",trace" << t + 1 << "_" << f->trace_modes[t];
        }
        os << "\n";
        for (std::size_t i = 0; i < f->points; ++i) {
            os << fmt::format("{:.1f},{:.2f}", point_freq(i, f->start_hz, f->stop_hz, f->points), f->live[i]);
            for (std::size_t t = 0; t < kTraces; ++t) {
                if (!f->traces[t].empty()) os << fmt::format(",{:.2f}", f->traces[t][i]);
            }
            os << "\n";
        }
        res.set_header("Content-Disposition", "attachment; filename=\"trace.csv\"");
        res.set_content(os.str(), "text/csv");
    });
    server.get("/api/iq.cs16", [this](const forge::http::Request&, forge::http::Response& res) {
        if (!engine_) throw HttpError(503, "not running");
        const auto c = engine_->last_capture();
        if (c.iq.empty()) throw HttpError(409, "no capture yet (IQ is kept for realtime spans only)");
        res.set_header("Content-Disposition", "attachment; filename=\"capture.cs16\"");
        res.set_header("X-Sample-Rate", fmt::format("{:.0f}", c.sample_rate_hz));
        res.set_header("X-Center-Frequency", fmt::format("{:.0f}", c.center_hz));
        res.set_content(std::string(reinterpret_cast<const char*>(c.iq.data()), c.iq.size() * sizeof(std::int16_t)),
                        "application/octet-stream");
    });
}

void Analyzer::setup_console(forge::Console& c) {
    c.add("peaks", "peaks  list the strongest signals", [this, &c](const forge::Console::Args&) {
        const auto f = latest();
        if (!f || f->peaks.empty()) return c.print("no peaks");
        for (const auto& p : f->peaks) c.print("  {:>18}  {:8.2f} {}", fmt_freq(p.freq_hz), p.db, f->unit);
    });
    c.add("markers", "markers  show marker readings", [this, &c](const forge::Console::Args&) {
        const auto f = latest();
        if (!f) return c.print("no sweep yet");
        for (std::size_t i = 0; i < kMarkers; ++i) {
            const auto& m = f->markers[i];
            if (!m.enabled) continue;
            if (m.outside) {
                c.print("  M{}  {:>18}  outside the span", i + 1, fmt_freq(m.freq_hz));
                continue;
            }
            c.print("  M{}  {:>18}  {:8.2f} {}{}", i + 1, fmt_freq(m.freq_hz), m.level, f->unit,
                    m.delta ? fmt::format("   delta {} / {:+.2f} dB", fmt_freq(m.dfreq_hz), m.dlevel_db) : "");
        }
    });
}

// ---------------------------------------------------------------------------
// lifecycle

void Analyzer::on_start() {
    const auto cfg = config();
    const auto kind = backend_kind_;
    const auto uri = uri_;
    const auto timeout = timeout_ms_;
    engine_ = std::make_unique<Engine>(
        [kind, uri, timeout]() -> std::unique_ptr<radio::Backend> {
            if (kind == "iio") return radio::make_iio_backend(uri, timeout);
            radio::SimOptions o;
            o.loopback = false;
            return radio::make_sim_backend(o);
        },
        limits_);
    engine_->start(settings_);

    control_ = forge::zmq::serve_application(*this, cfg.get_or<std::string>("zmq.control", "tcp://*:5567"));
    forge::zmq::SocketOptions so;
    so.send_hwm = 16;
    spectrum_pub_ = std::make_unique<forge::zmq::Sender>(
        forge::zmq::Sender::Kind::pub, cfg.get_or<std::string>("zmq.spectrum", "tcp://*:5566"), true, so);
    spectrum_endpoint_ = spectrum_pub_->endpoint();
    logger()->info("zmq: spectrum on {}, control on {}", spectrum_endpoint_, control_->endpoint());

    every(30ms, [this] { publish_frame(); });
    every(1s, [this] { check_status(); });
    every(2s, [this] { save_state(); });
}

void Analyzer::on_stop() {
    if (engine_) engine_->stop();
    control_.reset();
    spectrum_pub_.reset();
    dirty_ = true;
    save_state();
}

void Analyzer::publish_frame() {
    if (!spectrum_pub_) return;  // timers can still fire while shutting down
    const auto f = latest();
    if (!f || f->seq == last_seq_) return;
    last_seq_ = f->seq;

    // Peak track / signal track feed back into the settings (and the UI controls).
    if (settings_.peak_track) {
        const auto& r = f->markers[settings_.active_marker];
        if (r.enabled && !r.outside && std::abs(r.freq_hz - active().freq_hz) > 0.5) {
            active().freq_hz = r.freq_hz;
            changed();
        }
    }
    if (f->track_center_hz && settings_.signal_track) set_frequency(*f->track_center_hz, settings_.span_hz);

    // Limit line (display unit).
    if (limit_on_ && !f->live.empty()) {
        const auto it = std::max_element(f->live.begin(), f->live.end());
        const auto i = static_cast<std::size_t>(std::distance(f->live.begin(), it));
        // Raise once per crossing (1 dB hysteresis), not once per sweep.
        if (!limit_alarm_ && *it > limit_level_) {
            limit_alarm_ = true;
            alarms().raise(kLimitAlarm, forge::Severity::warning,
                           fmt::format("Signal at {} is {:.1f} dB above the limit ({:.1f} {})",
                                       fmt_freq(point_freq(i, f->start_hz, f->stop_hz, f->points)),
                                       *it - limit_level_, limit_level_, f->unit));
        } else if (limit_alarm_ && *it < limit_level_ - 1) {
            limit_alarm_ = false;
            alarms().clear(kLimitAlarm);
        }
    } else if (limit_alarm_) {
        limit_alarm_ = false;
        alarms().clear(kLimitAlarm);
    }
    const auto now = std::chrono::steady_clock::now();
    if (f->overload) {
        last_overload_ = now;
        if (!overload_alarm_) {
            overload_alarm_ = true;
            alarms().raise(kOverloadAlarm, forge::Severity::warning,
                           "ADC overload: the input is clipping, reduce the RX gain");
        }
    } else if (overload_alarm_ && now - last_overload_ > 3s) {
        overload_alarm_ = false;
        alarms().clear(kOverloadAlarm);
    }

    publish_web("frame", frame_json(*f));

    if (spectrum_pub_->subscriptions() > 0) {
        std::vector<float> data(f->live.begin(), f->live.end());
        spectrum_pub_->send("spectrum",
                            {{"seq", f->seq},
                             {"start_hz", f->start_hz},
                             {"stop_hz", f->stop_hz},
                             {"points", f->points},
                             {"unit", f->unit},
                             {"rbw_hz", f->plan.rbw_hz},
                             {"format", "f32"}},
                            data.data(), data.size() * sizeof(float));
    }
}

void Analyzer::check_status() {
    if (!engine_ || !spectrum_pub_) return;
    const bool was = status_.connected;
    status_ = engine_->status();
    if (status_.connected) {
        alarms().clear(kDeviceAlarm);
        if (!was) logger()->info("analyzer: connected to {}", status_.backend);
    } else {
        alarms().raise(kDeviceAlarm, forge::Severity::error, "Radio not available: " + status_.error);
    }
}

}  // namespace sa
