#include "app.hpp"

#include "sim.hpp"
#include "uio_source.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#ifndef LA_VERSION
#define LA_VERSION "0.0.0"
#endif

using namespace std::chrono_literals;
using forge::json;
using forge::http::HttpError;

namespace la {
namespace {

constexpr const char* kSourceAlarm = "la.source";
constexpr const char* kLostAlarm = "la.lost";

std::string rate_label(double hz) {
    if (hz >= 1e6) return std::format("{} MHz", hz / 1e6);
    return std::format("{} kHz", hz / 1e3);
}

std::vector<std::string> rate_labels() {
    std::vector<std::string> v;
    for (double r : sample_rates()) v.push_back(rate_label(r));
    return v;
}

struct LogicLevel {
    const char* label;
    double threshold;
};
const std::vector<LogicLevel>& logic_levels() {
    static const std::vector<LogicLevel> l{{"1.2 V", 0.6},  {"1.8 V", 0.9},     {"2.5 V", 1.25},
                                           {"3.3 V", 1.65}, {"5 V (TTL)", 1.4}, {"Custom", 0}};
    return l;
}

const std::vector<std::string>& trigger_types() {
    static const std::vector<std::string> t{"none", "rising", "falling", "any", "pattern"};
    return t;
}

std::uint64_t param_u64(const forge::http::Request& req, const char* name, std::uint64_t def) {
    if (!req.has_param(name)) return def;
    const auto s = req.get_param_value(name);
    try {
        const double d = std::stod(s);
        if (!(d >= 0)) throw std::invalid_argument("negative");
        return static_cast<std::uint64_t>(d);
    } catch (const std::exception&) {
        throw HttpError(400, std::format("{} must be a number", name));
    }
}

std::string lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

/// The few characters that fit in a small bubble.
std::string short_text(const std::string& type, const DecodedFrame& f, const std::string& radix) {
    switch (f.kind) {
        case FrameKind::start: return (f.flags & kRepeated) ? "Sr" : "S";
        case FrameKind::stop: return "P";
        case FrameKind::address:
            return std::format("{} {:02X}{}", (f.flags & kRead) ? "R" : "W", f.value, (f.flags & kNack) ? " N" : "");
        case FrameKind::error: return "!";
        case FrameKind::data: break;
    }
    if (type == "spi") {
        std::string s;
        if (!(f.flags & kNoMosi)) s += format_value(f.value, f.bits, radix);
        if (!(f.flags & kNoMiso)) s += (s.empty() ? "" : "/") + format_value(f.value2, f.bits, radix);
        return s;
    }
    std::string s = format_value(f.value, f.bits, radix);
    if (f.flags & (kParityError | kFramingError)) s += " !";
    return s;
}

std::string csv_quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + '"';
}

std::string attachment(const std::string& name) { return "attachment; filename=\"" + name + "\""; }

}  // namespace

LogicAnalyzer::LogicAnalyzer() : Application({"logic-analyzer", LA_VERSION, "Four-channel logic analyzer"}) {}

// ---------------------------------------------------------------------------
// configuration

void LogicAnalyzer::setup_config(forge::config::Schema& s) {
    s.section("analyzer", "Logic analyzer");
    s.string("analyzer.source", "sim", "sim (simulated signals) | uio (the FPGA core)").choices({"sim", "uio"});
    s.string("analyzer.scenario", "mixed", "sim: mixed (UART, I2C, PWM) | spi | uart")
        .choices({"mixed", "spi", "uart"});
    s.string("analyzer.uio_device", "logic-analyzer", "uio: /dev/uioN, or the UIO name from the device tree");
    s.string("analyzer.captures_dir", "captures", "Finished captures are saved here (.lacap)");
    s.integer("analyzer.keep_captures", 20, "Captures kept in captures_dir; the oldest go").range(1, 10000);
    s.integer("analyzer.max_edges", 4000000, "Changes per capture (9 bytes each); a capture stops at the limit")
        .range(1000, 100000000);
    s.string("analyzer.analyzers_file", "", "The protocol analyzers set up in the UI are kept here (JSON)");
    s.boolean("analyzer.start_on_boot", false, "Start a capture when the program starts");
}

void LogicAnalyzer::setup_cli(CLI::App& cli) {
    cli.add_option("--source", cli_source_, "sim | uio; overrides [analyzer].source")
        ->check(CLI::IsMember({"sim", "uio"}));
    cli.add_option("--scenario", cli_scenario_, "simulator: mixed | spi | uart")
        ->check(CLI::IsMember(scenario_names()));
}

void LogicAnalyzer::setup_controls(forge::Controls& ui) {
    auto& cap = ui.group("Capture");
    cap.button("start", "Start", [this] { start_capture(); });
    cap.button("stop", "Stop", [this] { stop_capture(); });
    cap.value("state", "State", [this] { return std::string(state_name(status_.state)); });
    cap.value("recorded_s", "Recorded", [this] { return status_.recorded_s; }).unit("s").precision(3);
    cap.select("sample_rate", "Sample rate", rate_labels(),
               [this] {
        std::lock_guard lock(mutex_);
        return rate_label_;
    },
               [this](const std::string& v) {
        const auto labels = rate_labels();
        const auto it = std::find(labels.begin(), labels.end(), v);
        if (it == labels.end()) throw HttpError(400, "not a sample rate: " + v);
        std::lock_guard lock(mutex_);
        rate_label_ = v;
        settings_.sample_hz = sample_rates()[static_cast<std::size_t>(it - labels.begin())];
    })
        .help("The FPGA divides its 100 MHz clock; the time resolution is one sample")
        .persist();
    cap.number("duration_s", "Duration",
               [this] {
        std::lock_guard lock(mutex_);
        return settings_.duration_s;
    },
               [this](double v) {
        std::lock_guard lock(mutex_);
        settings_.duration_s = v;
    })
        .range(0, 3600, 0.001)
        .unit("s")
        .help("Recorded after the trigger; 0 = until Stop")
        .persist();
    cap.select("trigger", "Trigger", trigger_types(), [this] {
        std::lock_guard lock(mutex_);
        return settings_.trigger;
    }, [this](const std::string& v) {
        std::lock_guard lock(mutex_);
        settings_.trigger = v;
    }).persist();
    cap.select("trigger_channel", "Trigger channel", {"0", "1", "2", "3"}, [this] {
        std::lock_guard lock(mutex_);
        return std::to_string(settings_.trigger_channel);
    }, [this](const std::string& v) {
        std::lock_guard lock(mutex_);
        settings_.trigger_channel = static_cast<unsigned>(std::stoi(v));
    }).persist();
    cap.text("pattern", "Trigger pattern",
             [this] {
        std::lock_guard lock(mutex_);
        return settings_.pattern;
    },
             [this](const std::string& v) {
        std::string p = v;
        for (auto& c : p) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (p.size() != kChannels || p.find_first_not_of("01X") != std::string::npos)
            throw HttpError(400, "pattern: four of 0, 1 or X (D0 first), e.g. 10XX");
        std::lock_guard lock(mutex_);
        settings_.pattern = p;
    })
        .help("D0..D3, each 0, 1 or X: fires on the first change into it")
        .persist();
    cap.number("pre_trigger_ms", "Pre-trigger",
               [this] {
        std::lock_guard lock(mutex_);
        return settings_.pre_trigger_ms;
    },
               [this](double v) {
        std::lock_guard lock(mutex_);
        settings_.pre_trigger_ms = v;
    })
        .range(0, 10000, 0.1)
        .unit("ms")
        .persist();
    cap.toggle("repeat", "Repeat",
               [this] {
        std::lock_guard lock(mutex_);
        return settings_.repeat;
    },
               [this](bool v) {
        std::lock_guard lock(mutex_);
        settings_.repeat = v;
    })
        .help("Arm again after each capture (like a scope's normal mode)")
        .persist();

    auto& in = ui.group("Inputs");
    std::vector<std::string> levels;
    for (const auto& l : logic_levels()) levels.push_back(l.label);
    in.select("logic_level", "Logic level", levels,
              [this] {
        std::lock_guard lock(mutex_);
        return logic_level_;
    },
              [this](const std::string& v) {
        for (const auto& l : logic_levels()) {
            if (v != l.label) continue;
            std::lock_guard lock(mutex_);
            logic_level_ = v;
            if (l.threshold > 0) settings_.threshold_v = l.threshold;
            return;
        }
        throw HttpError(400, "not a logic level: " + v);
    })
        .help("Sets the comparator threshold of the shield")
        .persist();
    in.number("threshold_v", "Threshold",
              [this] {
        std::lock_guard lock(mutex_);
        return settings_.threshold_v;
    },
              [this](double v) {
        std::lock_guard lock(mutex_);
        settings_.threshold_v = v;
        logic_level_ = "Custom";
        for (const auto& l : logic_levels()) {
            if (l.threshold > 0 && std::abs(l.threshold - v) < 1e-6) logic_level_ = l.label;
        }
    })
        .range(0.1, 3.2, 0.01)
        .unit("V")
        .precision(2)
        .persist();
    in.number("glitch_ns", "Glitch filter",
              [this] {
        std::lock_guard lock(mutex_);
        return static_cast<double>(settings_.glitch_ns);
    },
              [this](double v) {
        std::lock_guard lock(mutex_);
        settings_.glitch_ns = static_cast<std::uint32_t>(std::max(0.0, v));
    })
        .range(0, 100000, 10)
        .unit("ns")
        .help("Pulses shorter than this are ignored (done in the FPGA)")
        .persist();
    for (unsigned ch = 0; ch < kChannels; ++ch) {
        in.text(std::format("name{}", ch), std::format("D{} name", ch), [this, ch] {
            std::lock_guard lock(mutex_);
            return names_[ch];
        }, [this, ch](const std::string& v) {
            if (v.size() > 24) throw HttpError(400, "names are at most 24 characters");
            std::lock_guard lock(mutex_);
            names_[ch] = v.empty() ? std::format("D{}", ch) : v;
            if (engine_) engine_->set_names(names_);
        }).persist();
    }

    auto& st = ui.group("Status");
    st.value("source", "Source", [this] { return engine_ ? engine_->source().name() : std::string(); });
    st.value("words_per_s", "Words/s", [this] { return status_.words_per_s; }).precision(0);
    st.value("edges_per_s", "Changes/s", [this] { return status_.edges_per_s; }).precision(0);
    st.value("lost_words", "Words lost", [this] { return status_.source.lost; });
}

// ---------------------------------------------------------------------------
// analyzers

std::string LogicAnalyzer::next_id() { return std::format("a{}", next_analyzer_++); }

json LogicAnalyzer::analyzer_json(const AnalyzerDef& a) const {
    return {{"id", a.id},       {"type", a.type},      {"name", a.name}, {"settings", a.settings},
            {"radix", a.radix}, {"enabled", a.enabled}};
}

json LogicAnalyzer::analyzers_json() const {
    json a = json::array();
    for (const auto& d : analyzers_) a.push_back(analyzer_json(d));
    return a;
}

AnalyzerDef LogicAnalyzer::parse_analyzer(const json& body, AnalyzerDef a) const {
    if (!body.is_object()) throw HttpError(400, "an analyzer is an object");
    try {
        if (body.contains("type")) a.type = body["type"].get<std::string>();
        if (body.contains("name")) a.name = body["name"].get<std::string>();
        if (body.contains("radix")) a.radix = body["radix"].get<std::string>();
        if (body.contains("enabled")) a.enabled = body["enabled"].get<bool>();
        json settings = a.settings.is_object() ? a.settings : json::object();
        if (body.contains("settings")) {
            if (!body["settings"].is_object()) throw std::invalid_argument("settings must be an object");
            for (const auto& [k, v] : body["settings"].items()) settings[k] = v;
        }
        a.settings = normalize_settings(a.type, settings);
    } catch (const HttpError&) {
        throw;
    } catch (const std::exception& e) {
        throw HttpError(400, e.what());
    }
    if (a.radix != "hex" && a.radix != "dec" && a.radix != "bin" && a.radix != "ascii")
        throw HttpError(400, "radix: hex, dec, bin or ascii");
    if (a.name.empty()) {
        for (const auto& t : decoder_types()) {
            if (t["type"] == a.type) a.name = t["label"].get<std::string>();
        }
    }
    if (a.name.size() > 40) a.name.resize(40);
    return a;
}

void LogicAnalyzer::load_analyzers() {
    json list;
    if (!analyzers_file_.empty() && std::filesystem::exists(analyzers_file_)) {
        try {
            std::ifstream in(analyzers_file_);
            list = json::parse(in);
        } catch (const std::exception& e) {
            logger()->warn("analyzers: cannot read {}: {}", analyzers_file_, e.what());
        }
    }
    if (!list.is_array()) list = engine_ ? engine_->source().suggested_analyzers() : json::array();
    std::lock_guard lock(mutex_);
    analyzers_.clear();
    for (const auto& j : list) {
        try {
            AnalyzerDef a = parse_analyzer(j, AnalyzerDef{});
            a.id = j.contains("id") ? j["id"].get<std::string>() : next_id();
            analyzers_.push_back(std::move(a));
        } catch (const std::exception& e) {
            logger()->warn("analyzers: skipped one: {}", e.what());
        }
    }
    for (const auto& a : analyzers_) {  // ids after the ones loaded
        if (a.id.size() > 1)
            next_analyzer_ = std::max(next_analyzer_, static_cast<unsigned>(std::atoi(a.id.c_str() + 1)) + 1);
    }
    ++analyzers_version_;
}

void LogicAnalyzer::save_analyzers() {
    if (analyzers_file_.empty()) return;
    json list;
    {
        std::lock_guard lock(mutex_);
        list = analyzers_json();
    }
    const std::string tmp = analyzers_file_ + ".tmp";
    {
        std::ofstream out(tmp);
        out << list.dump(2) << '\n';
    }
    std::error_code ec;
    std::filesystem::rename(tmp, analyzers_file_, ec);
    if (ec) logger()->warn("analyzers: cannot write {}: {}", analyzers_file_, ec.message());
}

std::shared_ptr<SharedCapture> LogicAnalyzer::shown() const {
    {
        std::lock_guard lock(mutex_);
        if (loaded_) return loaded_;
    }
    return engine_ ? engine_->current() : nullptr;
}

CaptureSettings LogicAnalyzer::settings() const {
    std::lock_guard lock(mutex_);
    return settings_;
}

std::shared_ptr<const Decoded> LogicAnalyzer::decoded() {
    const auto cap = shown();
    if (!cap) return nullptr;
    std::lock_guard dl(decode_mutex_);
    const auto [edges, done] = cap->read([](const Capture& c) { return std::pair(c.size(), c.done); });
    std::vector<AnalyzerDef> defs;
    std::uint64_t version;
    {
        std::lock_guard lock(mutex_);
        defs = analyzers_;
        version = analyzers_version_;
    }
    if (decoded_ && decoded_->capture == cap.get() && decoded_->version == version) {
        if (decoded_->edges == edges && decoded_->done == done) return decoded_;
        // still recording: decode again at most three times a second
        if (!done && std::chrono::steady_clock::now() - decoded_->at < 300ms) return decoded_;
    }
    auto d = std::make_shared<Decoded>();
    d->capture = cap.get();
    d->version = version;
    d->at = std::chrono::steady_clock::now();
    cap->read([&](const Capture& c) {
        d->edges = c.size();
        d->done = c.done;
        for (const auto& a : defs) {
            if (!a.enabled) continue;
            try {
                auto dec = make_decoder(a.type, a.settings);
                d->items.push_back({a, dec->display_channel(), dec->decode(c)});
            } catch (const std::exception& e) {
                logger()->warn("analyzer {}: {}", a.name, e.what());
            }
        }
        return 0;
    });
    std::size_t total = 0;
    for (const auto& it : d->items) total += it.frames.size();
    d->order.reserve(total);
    for (std::uint32_t i = 0; i < d->items.size(); ++i) {
        for (std::uint32_t k = 0; k < d->items[i].frames.size(); ++k) d->order.emplace_back(i, k);
    }
    std::stable_sort(d->order.begin(), d->order.end(), [&](const auto& a, const auto& b) {
        return d->items[a.first].frames[a.second].start < d->items[b.first].frames[b.second].start;
    });
    decoded_ = d;
    return decoded_;
}

// ---------------------------------------------------------------------------
// capture

void LogicAnalyzer::start_capture() {
    if (!engine_) throw HttpError(503, "not running");
    CaptureSettings s;
    {
        std::lock_guard lock(mutex_);
        loaded_.reset();  // back to live
        loaded_file_.clear();
        s = settings_;
    }
    if (s.trigger == "pattern" && s.pattern.find_first_of("01") == std::string::npos)
        throw HttpError(400, "the pattern has no 0 or 1 in it: it would never fire");
    engine_->set_names(names_);
    engine_->start(s);
    logger()->info("capture: {} at {}, {}, trigger {}",
                   s.duration_s > 0 ? std::format("{} s", s.duration_s) : "until stopped", rate_label(s.sample_hz),
                   s.repeat ? "repeating" : "once", s.trigger);
}

void LogicAnalyzer::stop_capture() {
    if (engine_) engine_->stop();
}

void LogicAnalyzer::captured(std::shared_ptr<SharedCapture> shared) {
    // Engine thread: save the file here (it can take a moment), report on the main loop.
    json summary = shared->read([](const Capture& c) { return capture_summary(c); });
    std::string file;
    std::string error;
    if (store_ && summary["edges"].get<std::size_t>() + 1 > 0) {
        try {
            json extra;
            {
                std::lock_guard lock(mutex_);
                extra = {{"analyzers", analyzers_json()}};
            }
            file = shared->read([&](const Capture& c) { return store_->save(c, extra); });
        } catch (const std::exception& e) {
            error = e.what();
        }
    }
    post([this, summary, file, error] {
        static auto& captures = metrics().counter("la.captures", "Captures finished");
        captures.inc();
        if (!error.empty()) logger()->warn("capture: not saved: {}", error);
        {
            std::lock_guard lock(mutex_);
            last_file_ = file;
        }
        const auto st = engine_ ? engine_->status() : EngineStatus{};
        logger()->info("capture {} done ({}): {:.3f} s, {} changes{}", summary["id"].get<std::uint64_t>(),
                       st.stop_reason, summary["duration_s"].get<double>(), summary["edges"].get<std::size_t>(),
                       file.empty() ? "" : ", saved as " + file);
        json event = summary;
        event["file"] = file;
        event["reason"] = st.stop_reason;
        publish_web("capture", event);
    });
}

void LogicAnalyzer::check_status() {
    if (!engine_) return;
    status_ = engine_->status();
    if (!status_.error.empty()) {
        alarms().raise(kSourceAlarm, forge::Severity::error, "Source failed: " + status_.error);
    } else if (status_.state != State::idle) {
        alarms().clear(kSourceAlarm);
    }
    if (status_.source.lost > last_lost_) {
        alarms().raise(kLostAlarm, forge::Severity::warning,
                       std::format("{} words lost: the buffer was full (lower the sample rate, or fewer changes)",
                                   status_.source.lost));
    } else if (status_.state == State::idle) {
        alarms().clear(kLostAlarm);
    }
    last_lost_ = status_.source.lost;
    static auto& words = metrics().gauge("la.words_per_s", "Words read per second");
    static auto& edges = metrics().gauge("la.changes_per_s", "Changes recorded per second");
    words.set(status_.words_per_s);
    edges.set(status_.edges_per_s);
}

json LogicAnalyzer::status_json() {
    const auto st = engine_ ? engine_->status() : EngineStatus{};
    json j;
    j["state"] = state_name(st.state);
    j["recorded_s"] = st.recorded_s;
    j["words_per_s"] = st.words_per_s;
    j["edges_per_s"] = st.edges_per_s;
    j["lost_words"] = st.source.lost;
    j["error"] = st.error;
    j["stop_reason"] = st.stop_reason;
    {
        std::lock_guard lock(mutex_);
        j["settings"] = {{"sample_hz", settings_.sample_hz}, {"duration_s", settings_.duration_s},
                         {"trigger", settings_.trigger},     {"trigger_channel", settings_.trigger_channel},
                         {"pattern", settings_.pattern},     {"pre_trigger_ms", settings_.pre_trigger_ms},
                         {"repeat", settings_.repeat},       {"threshold_v", settings_.threshold_v},
                         {"glitch_ns", settings_.glitch_ns}, {"max_edges", settings_.max_edges}};
        j["names"] = names_;
        j["loaded_file"] = loaded_file_;
        j["last_file"] = last_file_;
    }
    if (engine_) {
        j["source"] = {{"name", engine_->source().name()}, {"info", engine_->source().info()}};
    }
    const auto cap = shown();
    j["capture"] = cap ? cap->read([](const Capture& c) {
        json s = capture_summary(c);
        s["origin"] = c.origin();
        return s;
    })
                       : json(nullptr);
    return j;
}

// ---------------------------------------------------------------------------
// HTTP

void LogicAnalyzer::setup_http(forge::http::Server& server) {
    using Req = forge::http::Request;
    using Res = forge::http::Response;

    server.get_json("/api/status", [this](const Req&) { return status_json(); });
    server.get_json("/api/meta", [](const Req&) {
        json levels = json::array();
        for (const auto& l : logic_levels()) levels.push_back({{"label", l.label}, {"threshold", l.threshold}});
        return json{{"sample_rates", sample_rates()}, {"rate_labels", rate_labels()}, {"triggers", trigger_types()},
                    {"logic_levels", levels},         {"decoders", decoder_types()},  {"channels", kChannels}};
    });

    // The waveforms of [from, to] (ticks) for a display `px` wide.
    server.get_json("/api/view", [this](const Req& req) {
        const auto cap = shown();
        if (!cap) throw HttpError(409, "no capture yet");
        return cap->read([&](const Capture& c) {
            const std::uint64_t from = param_u64(req, "from", c.begin);
            const std::uint64_t to = param_u64(req, "to", c.end);
            const auto px = static_cast<unsigned>(std::clamp<std::uint64_t>(param_u64(req, "px", 1000), 1, 8192));
            const View v = make_view(c, from, std::max(from, to), px);
            json ch = json::array();
            for (const auto& cv : v.channels) {
                json items = json::array();
                for (const auto& it : cv.items) items.push_back({it.t0, it.t1, it.count, it.level ? 1 : 0});
                ch.push_back({{"initial", cv.initial ? 1 : 0}, {"items", items}});
            }
            return json{{"id", c.id},   {"from", v.from}, {"to", v.to},
                        {"end", c.end}, {"channels", ch}, {"gaps", v.gaps}};
        });
    });

    // Decoded frames in [from, to] for the bubbles; dense parts as blocks.
    server.get_json("/api/frames", [this](const Req& req) {
        const auto d = decoded();
        json out = json::array();
        if (!d) return json{{"analyzers", out}};
        const std::uint64_t from = param_u64(req, "from", 0);
        const std::uint64_t to = param_u64(req, "to", std::numeric_limits<std::uint64_t>::max());
        const auto px = static_cast<double>(std::clamp<std::uint64_t>(param_u64(req, "px", 1000), 1, 8192));
        const double per_px = std::max(1.0, static_cast<double>(to - from) / px);
        for (const auto& it : d->items) {
            const auto& fr = it.frames;
            auto i = static_cast<std::size_t>(
                std::lower_bound(fr.begin(), fr.end(), from, [](const DecodedFrame& f, std::uint64_t t) {
                return f.end < t;
            }) - fr.begin());
            json frames = json::array(), blocks = json::array();
            std::size_t n = 0;
            for (std::size_t k = i; k < fr.size() && fr[k].start <= to; ++k) ++n;
            const bool dense = static_cast<double>(n) > px / 4;  // fewer than 4 px each: draw blocks
            for (; i < fr.size() && fr[i].start <= to; ++i) {
                const auto& f = fr[i];
                if (dense) {
                    if (!blocks.empty() && static_cast<double>(f.start) - blocks.back()[1].get<double>() < 2 * per_px) {
                        blocks.back()[1] = f.end;
                        blocks.back()[2] = blocks.back()[2].get<std::uint64_t>() + 1;
                    } else {
                        blocks.push_back({f.start, f.end, 1});
                    }
                    continue;
                }
                const bool err = f.kind == FrameKind::error || (f.flags & (kParityError | kFramingError));
                frames.push_back({f.start, f.end, kind_name(f.kind), err ? 1 : 0,
                                  short_text(it.def.type, f, it.def.radix), describe(it.def.type, f, it.def.radix)});
            }
            out.push_back({{"id", it.def.id},
                           {"name", it.def.name},
                           {"type", it.def.type},
                           {"channel", it.channel},
                           {"count", fr.size()},
                           {"frames", frames},
                           {"blocks", blocks}});
        }
        return json{{"analyzers", out}};
    });

    // The data table: every analyzer's frames by time, filtered and paged.
    server.get_json("/api/table", [this](const Req& req) {
        const auto d = decoded();
        if (!d) return json{{"total", 0}, {"rows", json::array()}, {"offset", 0}};
        const std::string q = lower(req.has_param("q") ? req.get_param_value("q") : "");
        const std::string only = req.has_param("analyzer") ? req.get_param_value("analyzer") : "";
        const std::size_t limit = std::clamp<std::uint64_t>(param_u64(req, "limit", 200), 1, 5000);
        std::size_t offset = param_u64(req, "offset", 0);
        const auto origin = shown()->read([](const Capture& c) { return std::pair(c.origin(), c.tick_hz); });
        const auto row = [&](const Decoded::Item& it, const DecodedFrame& f) {
            return json{{"analyzer", it.def.id},
                        {"name", it.def.name},
                        {"start", f.start},
                        {"end", f.end},
                        {"time_s", (static_cast<double>(f.start) - static_cast<double>(origin.first)) / origin.second},
                        {"duration_s", static_cast<double>(f.end - f.start) / origin.second},
                        {"kind", kind_name(f.kind)},
                        {"error", f.kind == FrameKind::error || (f.flags & (kParityError | kFramingError)) != 0},
                        {"text", describe(it.def.type, f, it.def.radix)}};
        };
        // Rows that pass the filters, as indexes into d->order.
        std::vector<std::uint32_t> pass;
        const bool filtered = !q.empty() || !only.empty();
        if (filtered) {
            for (std::uint32_t i = 0; i < d->order.size(); ++i) {
                const auto& [a, k] = d->order[i];
                const auto& it = d->items[a];
                if (!only.empty() && it.def.id != only) continue;
                if (!q.empty()) {
                    const auto& f = it.frames[k];
                    const std::string text = lower(describe(it.def.type, f, it.def.radix) + " " + it.def.name + " " +
                                                   format_value(f.value, f.bits, "hex"));
                    if (text.find(q) == std::string::npos) continue;
                }
                pass.push_back(i);
            }
        }
        const std::size_t total = filtered ? pass.size() : d->order.size();
        if (req.has_param("at")) {  // the page with the first row at or after this tick
            const std::uint64_t at = param_u64(req, "at", 0);
            std::size_t idx = 0;
            for (; idx < total; ++idx) {
                const auto& [a, k] = d->order[filtered ? pass[idx] : idx];
                if (d->items[a].frames[k].start >= at) break;
            }
            offset = idx >= limit / 2 ? idx - limit / 2 : 0;
        }
        json rows = json::array();
        for (std::size_t i = offset; i < total && rows.size() < limit; ++i) {
            const auto& [a, k] = d->order[filtered ? pass[i] : i];
            json r = row(d->items[a], d->items[a].frames[k]);
            r["index"] = i;
            rows.push_back(std::move(r));
        }
        return json{{"total", total}, {"offset", offset}, {"rows", rows}};
    });

    // Measurements of each channel between two times (cursors or a selection).
    server.get_json("/api/measure", [this](const Req& req) {
        const auto cap = shown();
        if (!cap) throw HttpError(409, "no capture yet");
        return cap->read([&](const Capture& c) {
            const std::uint64_t from = param_u64(req, "from", c.begin);
            const std::uint64_t to = param_u64(req, "to", c.end);
            json ch = json::array();
            const auto opt = [](const std::optional<double>& v) { return v ? json(*v) : json(nullptr); };
            for (unsigned i = 0; i < kChannels; ++i) {
                const auto s = measure(c, i, std::min(from, to), std::max(from, to));
                ch.push_back({{"rising", s.rising},
                              {"falling", s.falling},
                              {"frequency_hz", opt(s.frequency_hz())},
                              {"period_s", opt(s.period_s)},
                              {"period_min_s", opt(s.period_min_s)},
                              {"period_max_s", opt(s.period_max_s)},
                              {"duty", opt(s.duty())},
                              {"high_min_s", opt(s.width_high_min_s)},
                              {"high_max_s", opt(s.width_high_max_s)},
                              {"low_min_s", opt(s.width_low_min_s)},
                              {"low_max_s", opt(s.width_low_max_s)}});
            }
            return json{{"from", from},
                        {"to", to},
                        {"seconds", c.seconds(std::max(from, to) - std::min(from, to))},
                        {"channels", ch}};
        });
    });

    // --- analyzers ---
    server.get_json("/api/analyzers", [this](const Req&) {
        std::lock_guard lock(mutex_);
        return analyzers_json();
    });
    server.post_json("/api/analyzers", [this](const Req&, const json& body) {
        AnalyzerDef a = parse_analyzer(body, AnalyzerDef{});
        json out;
        {
            std::lock_guard lock(mutex_);
            if (analyzers_.size() >= 16) throw HttpError(409, "at most 16 analyzers");
            a.id = next_id();
            analyzers_.push_back(a);
            ++analyzers_version_;
            out = analyzer_json(a);
        }
        save_analyzers();
        return out;
    });
    server.put_json("/api/analyzers/:id", [this](const Req& req, const json& body) {
        const auto id = req.path_params.at("id");
        json out;
        {
            std::lock_guard lock(mutex_);
            auto it = std::find_if(analyzers_.begin(), analyzers_.end(), [&](const auto& a) { return a.id == id; });
            if (it == analyzers_.end()) throw HttpError(404, "no analyzer " + id);
            AnalyzerDef base = *it;
            if (body.contains("type") && body["type"] != base.type) base.settings = json::object();  // other fields
            *it = parse_analyzer(body, base);
            ++analyzers_version_;
            out = analyzer_json(*it);
        }
        save_analyzers();
        return out;
    });
    server.del_json("/api/analyzers/:id", [this](const Req& req) {
        const auto id = req.path_params.at("id");
        {
            std::lock_guard lock(mutex_);
            const auto n = std::erase_if(analyzers_, [&](const auto& a) { return a.id == id; });
            if (n == 0) throw HttpError(404, "no analyzer " + id);
            ++analyzers_version_;
        }
        save_analyzers();
        return json(nullptr);
    });

    // --- captures on disk ---
    server.get_json("/api/captures", [this](const Req&) { return store_ ? store_->list() : json::array(); });
    server.post_json("/api/captures/load", [this](const Req&, const json& body) {
        if (!body.is_object() || !body.contains("file"))
            throw HttpError(400, "body: {\"file\": \"capture-....lacap\"}");
        const std::string file = body["file"].get<std::string>();
        if (engine_ && engine_->status().state != State::idle) throw HttpError(409, "stop the capture first");
        try {
            auto c = std::make_shared<SharedCapture>(store_->load(file));
            std::lock_guard lock(mutex_);
            loaded_ = c;
            loaded_file_ = file;
        } catch (const std::invalid_argument& e) {
            throw HttpError(404, e.what());
        } catch (const std::exception& e) {
            throw HttpError(422, e.what());
        }
        return status_json();
    });
    server.post_json("/api/captures/live", [this](const Req&, const json&) {
        std::lock_guard lock(mutex_);
        loaded_.reset();
        loaded_file_.clear();
        return json(nullptr);
    });
    server.post("/api/captures/import", [this](const Req& req, Res& res) {
        std::istringstream in(req.body);
        Capture c;
        try {
            c = read_capture(in);
        } catch (const std::exception& e) {
            throw HttpError(422, e.what());
        }
        const std::string file = store_->save(c);
        {
            std::lock_guard lock(mutex_);
            loaded_ = std::make_shared<SharedCapture>(store_->load(file));
            loaded_file_ = file;
        }
        res.set_content(json{{"file", file}}.dump(), "application/json");
    });
    server.get("/api/captures/:file", [this](const Req& req, Res& res) {
        const auto file = req.path_params.at("file");
        std::filesystem::path p;
        try {
            p = store_->path(file);
        } catch (const std::exception& e) {
            throw HttpError(400, e.what());
        }
        std::ifstream in(p, std::ios::binary);
        if (!in) throw HttpError(404, "no capture " + file);
        std::ostringstream os;
        os << in.rdbuf();
        res.set_header("Content-Disposition", attachment(file));
        res.set_content(os.str(), "application/octet-stream");
    });
    server.del_json("/api/captures/:file", [this](const Req& req) {
        const auto file = req.path_params.at("file");
        try {
            if (!store_->remove(file)) throw HttpError(404, "no capture " + file);
        } catch (const std::invalid_argument& e) {
            throw HttpError(400, e.what());
        }
        return json(nullptr);
    });

    // --- exports of the capture shown ---
    const auto export_name = [this](const char* ext) {
        const auto cap = shown();
        const auto ms = cap ? cap->read([](const Capture& c) { return c.started_ms; }) : 0;
        const auto t = std::chrono::sys_time<std::chrono::milliseconds>(std::chrono::milliseconds(ms));
        return std::format("capture-{:%Y%m%d-%H%M%S}.{}", std::chrono::floor<std::chrono::seconds>(t), ext);
    };
    const auto need = [this] {
        const auto cap = shown();
        if (!cap) throw HttpError(409, "no capture yet");
        return cap;
    };
    server.get("/api/export/capture.lacap", [this, need, export_name](const Req&, Res& res) {
        std::ostringstream os;
        json extra;
        {
            std::lock_guard lock(mutex_);
            extra = {{"analyzers", analyzers_json()}};
        }
        need()->read([&](const Capture& c) {
            write_capture(c, os, extra);
            return 0;
        });
        res.set_header("Content-Disposition", attachment(export_name("lacap")));
        res.set_content(os.str(), "application/octet-stream");
    });
    server.get("/api/export/capture.vcd", [need, export_name](const Req&, Res& res) {
        std::ostringstream os;
        need()->read([&](const Capture& c) {
            write_vcd(c, os);
            return 0;
        });
        res.set_header("Content-Disposition", attachment(export_name("vcd")));
        res.set_content(os.str(), "text/plain");
    });
    server.get("/api/export/capture.csv", [need, export_name](const Req&, Res& res) {
        std::ostringstream os;
        need()->read([&](const Capture& c) {
            write_csv(c, os);
            return 0;
        });
        res.set_header("Content-Disposition", attachment(export_name("csv")));
        res.set_content(os.str(), "text/csv");
    });
    // The protocol log: every decoded frame, CSV (spreadsheets) or text (reading).
    server.get("/api/export/decoded.csv", [this, need, export_name](const Req& req, Res& res) {
        need();
        const auto d = decoded();
        const std::string only = req.has_param("analyzer") ? req.get_param_value("analyzer") : "";
        const auto origin = shown()->read([](const Capture& c) { return std::pair(c.origin(), c.tick_hz); });
        std::ostringstream os;
        os << "analyzer,type,kind,start_s,duration_s,value,text,error\n";
        for (const auto& [a, k] : d->order) {
            const auto& it = d->items[a];
            if (!only.empty() && it.def.id != only) continue;
            const auto& f = it.frames[k];
            const bool err = f.kind == FrameKind::error || (f.flags & (kParityError | kFramingError));
            os << csv_quote(it.def.name) << ',' << it.def.type << ',' << kind_name(f.kind) << ','
               << std::format("{:.9f},{:.9f}",
                              (static_cast<double>(f.start) - static_cast<double>(origin.first)) / origin.second,
                              static_cast<double>(f.end - f.start) / origin.second)
               << ',' << format_value(f.value, f.bits, "hex") << ','
               << csv_quote(describe(it.def.type, f, it.def.radix)) << ',' << (err ? 1 : 0) << '\n';
        }
        res.set_header("Content-Disposition", attachment(export_name("decoded.csv")));
        res.set_content(os.str(), "text/csv");
    });
    server.get("/api/export/decoded.txt", [this, need, export_name](const Req& req, Res& res) {
        need();
        const auto d = decoded();
        const std::string only = req.has_param("analyzer") ? req.get_param_value("analyzer") : "";
        const auto origin = shown()->read([](const Capture& c) { return std::pair(c.origin(), c.tick_hz); });
        std::ostringstream os;
        for (const auto& [a, k] : d->order) {
            const auto& it = d->items[a];
            if (!only.empty() && it.def.id != only) continue;
            const auto& f = it.frames[k];
            os << std::format("{:>16.9f}  {:<12}  {}\n",
                              (static_cast<double>(f.start) - static_cast<double>(origin.first)) / origin.second,
                              it.def.name, describe(it.def.type, f, it.def.radix));
        }
        res.set_header("Content-Disposition", attachment(export_name("decoded.txt")));
        res.set_content(os.str(), "text/plain");
    });
    // What a UART said, as text (a terminal's view).
    server.get_json("/api/terminal", [this](const Req& req) {
        const auto d = decoded();
        const std::string id = req.has_param("analyzer") ? req.get_param_value("analyzer") : "";
        json out = json::array();
        if (!d) return out;
        for (const auto& it : d->items) {
            if (it.def.type != "uart" || (!id.empty() && it.def.id != id)) continue;
            std::string text;
            for (const auto& f : it.frames) {
                if (f.kind != FrameKind::data || (f.flags & kBreak)) continue;
                const auto v = f.value & 0xFF;
                if (v == '\n' || v == '\t' || (v >= 0x20 && v < 0x7F)) {
                    text += static_cast<char>(v);
                } else if (v != '\r') {
                    text += std::format("\\x{:02X}", v);
                }
            }
            if (text.size() > 200000) text.erase(0, text.size() - 200000);  // the end of it
            out.push_back({{"id", it.def.id}, {"name", it.def.name}, {"text", text}});
        }
        return out;
    });
}

// ---------------------------------------------------------------------------
// console

void LogicAnalyzer::setup_console(forge::Console& c) {
    c.add("capture", "capture [start|stop]  start or stop a capture (default: start)",
          [this, &c](const forge::Console::Args& a) {
        if (!a.empty() && a[0] == "stop") {
            stop_capture();
            return c.print("stopping");
        }
        start_capture();
        c.print("started");
    });
    c.add("status", "status  the capture and the source", [this, &c](const forge::Console::Args&) {
        const json s = status_json();
        c.print("state {}  recorded {:.3f} s  {:.0f} words/s  {:.0f} changes/s  lost {}", s["state"].get<std::string>(),
                s["recorded_s"].get<double>(), s["words_per_s"].get<double>(), s["edges_per_s"].get<double>(),
                s["lost_words"].get<std::uint64_t>());
        if (!s["capture"].is_null()) {
            const auto& cap = s["capture"];
            c.print("capture {}  {:.6f} s  {} changes  at {}  {}", cap["id"].get<std::uint64_t>(),
                    cap["duration_s"].get<double>(), cap["edges"].get<std::size_t>(),
                    rate_label(cap["tick_hz"].get<double>()), cap["trigger"].is_null() ? "no trigger" : "triggered");
        }
        if (!s["error"].get<std::string>().empty()) c.print("error: {}", s["error"].get<std::string>());
    });
    c.add("analyzers", "analyzers  the protocol analyzers", [this, &c](const forge::Console::Args&) {
        std::lock_guard lock(mutex_);
        for (const auto& a : analyzers_)
            c.print("  {}  {:<5} {:<16} {}{}", a.id, a.type, a.name, a.settings.dump(), a.enabled ? "" : " (off)");
    });
    c.add("decode", "decode [analyzer id] [n]  the last n decoded frames (default 20)",
          [this, &c](const forge::Console::Args& a) {
        std::string id;
        std::size_t n = 20;
        for (const auto& w : a) {
            if (!w.empty() && std::isdigit(static_cast<unsigned char>(w[0]))) {
                n = std::stoul(w);
            } else {
                id = w;
            }
        }
        const auto d = decoded();
        if (!d) return c.print("no capture yet");
        const auto origin = shown()->read([](const Capture& cap) { return std::pair(cap.origin(), cap.tick_hz); });
        std::vector<std::pair<std::uint32_t, std::uint32_t>> rows;
        for (const auto& p : d->order) {
            if (id.empty() || d->items[p.first].def.id == id) rows.push_back(p);
        }
        const std::size_t from = rows.size() > n ? rows.size() - n : 0;
        for (std::size_t i = from; i < rows.size(); ++i) {
            const auto& it = d->items[rows[i].first];
            const auto& f = it.frames[rows[i].second];
            c.print("{:>14.6f} s  {:<12} {}",
                    (static_cast<double>(f.start) - static_cast<double>(origin.first)) / origin.second, it.def.name,
                    describe(it.def.type, f, it.def.radix));
        }
    });
}

// ---------------------------------------------------------------------------
// lifecycle

void LogicAnalyzer::on_start() {
    const auto cfg = config();
    source_kind_ = cli_source_.empty() ? cfg.value<std::string>("analyzer.source") : cli_source_;
    scenario_ = cli_scenario_.empty() ? cfg.value<std::string>("analyzer.scenario") : cli_scenario_;
    uio_device_ = cfg.value<std::string>("analyzer.uio_device");
    captures_dir_ = cfg.value<std::string>("analyzer.captures_dir");
    keep_captures_ = cfg.value<std::size_t>("analyzer.keep_captures");
    analyzers_file_ = cfg.value<std::string>("analyzer.analyzers_file");
    start_on_boot_ = cfg.value<bool>("analyzer.start_on_boot");
    {
        std::lock_guard lock(mutex_);
        settings_.max_edges = cfg.value<std::size_t>("analyzer.max_edges");
    }

    std::unique_ptr<Source> source;
    if (source_kind_ == "uio") {
        source = std::make_unique<UioSource>(uio_device_);
    } else {
        source = std::make_unique<SimSource>(make_scenario(scenario_));
        // Signal names of the simulator, unless the user named the channels.
        const auto names = source->channel_names();
        std::lock_guard lock(mutex_);
        for (unsigned ch = 0; ch < kChannels && ch < names.size(); ++ch) {
            if (names_[ch] == std::format("D{}", ch)) names_[ch] = names[ch];
        }
    }
    try {
        store_ = std::make_unique<CaptureStore>(captures_dir_, keep_captures_);
    } catch (const std::exception& e) {
        logger()->warn("captures: {} not usable ({}): captures are not saved", captures_dir_, e.what());
    }
    engine_ = std::make_unique<Engine>(std::move(source),
                                       [this](std::shared_ptr<SharedCapture> c) { captured(std::move(c)); });
    engine_->set_names(names_);
    load_analyzers();
    logger()->info("logic analyzer: source {}, captures in {}", engine_->source().name(), captures_dir_);

    every(250ms, [this] { check_status(); });
    if (start_on_boot_) start_capture();
}

void LogicAnalyzer::on_stop() {
    if (engine_) engine_->stop();
    engine_.reset();  // joins the engine's thread (it may still save the last capture)
}

}  // namespace la
