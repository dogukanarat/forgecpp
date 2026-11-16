#include "files.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <stdexcept>

namespace la {

using forge::json;
namespace fs = std::filesystem;

namespace {

constexpr const char* kMagic = "LACAP1";

std::string utc(std::int64_t ms, const char* fmt) {
    const auto t = std::chrono::floor<std::chrono::seconds>(
        std::chrono::sys_time<std::chrono::milliseconds>(std::chrono::milliseconds(ms)));
    return std::vformat(fmt, std::make_format_args(t));
}

}  // namespace

json capture_summary(const Capture& c) {
    json j = {{"id", c.id},
              {"started_ms", c.started_ms},
              {"started", utc(c.started_ms, "{:%Y-%m-%d %H:%M:%S}")},
              {"tick_hz", c.tick_hz},
              {"begin", c.begin},
              {"end", c.end},
              {"duration_s", c.seconds(c.end - c.begin)},
              {"edges", c.size()},
              {"names", c.names},
              {"source", c.source},
              {"truncated", c.truncated},
              {"done", c.done},
              {"gaps", c.gaps.size()}};
    j["trigger"] = c.trigger ? json(*c.trigger) : json(nullptr);
    return j;
}

void write_capture(const Capture& c, std::ostream& out, const json& extra) {
    json h = capture_summary(c);
    h.erase("started");
    h.erase("duration_s");
    h.erase("done");
    h["gaps"] = c.gaps;
    h["format"] = "words: 32-bit little endian, fpga/README.md; ticks relative to begin";
    h["extra"] = extra;
    out << kMagic << '\n' << h.dump() << '\n';

    std::vector<std::uint32_t> words;
    words.reserve(c.size() + 16);
    WireEncoder enc(words);
    enc.start(c.initial);
    std::size_t g = 0;
    for (std::size_t i = 0; i < c.size(); ++i) {
        const std::uint64_t t = c.t[i];
        for (; g < c.gaps.size() && c.gaps[g].second <= t; ++g) {
            const auto gt = std::max(c.gaps[g].second, c.begin) - c.begin;
            if (gt >= enc.tick()) enc.gap(gt, c.levels_at(c.gaps[g].second));
        }
        enc.change(t - c.begin, c.lv[i]);
    }
    enc.time(c.end - c.begin);
    for (const std::uint32_t w : words) {  // little endian whatever the host
        const char b[4] = {static_cast<char>(w), static_cast<char>(w >> 8), static_cast<char>(w >> 16),
                           static_cast<char>(w >> 24)};
        out.write(b, 4);
    }
}

Capture read_capture(std::istream& in, json* extra) {
    std::string magic, header;
    std::getline(in, magic);
    if (magic != kMagic) throw std::runtime_error("not a capture file (.lacap)");
    std::getline(in, header);
    json h;
    try {
        h = json::parse(header);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("capture file: bad header: ") + e.what());
    }
    Capture c;
    try {
        c.id = h.value("id", 0ull);
        c.started_ms = h.value("started_ms", 0ll);
        c.tick_hz = h.at("tick_hz").get<double>();
        c.begin = h.at("begin").get<std::uint64_t>();
        c.end = h.at("end").get<std::uint64_t>();
        if (h.contains("trigger") && !h["trigger"].is_null()) c.trigger = h["trigger"].get<std::uint64_t>();
        if (h.contains("names")) c.names = h["names"].get<std::array<std::string, kChannels>>();
        c.source = h.value("source", std::string("file"));
        c.truncated = h.value("truncated", false);
        if (h.contains("gaps")) c.gaps = h["gaps"].get<std::vector<std::pair<std::uint64_t, std::uint64_t>>>();
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("capture file: bad header: ") + e.what());
    }
    if (!(c.tick_hz > 0) || c.end < c.begin) throw std::runtime_error("capture file: bad header");
    if (extra) *extra = h.value("extra", json::object());

    std::vector<std::uint32_t> words;
    char b[4];
    while (in.read(b, 4)) {
        const auto u = [&](int i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(b[i])); };
        words.push_back(u(0) | (u(1) << 8) | (u(2) << 16) | (u(3) << 24));
    }
    WireDecoder dec;
    bool started = false;
    dec.feed(words, [&](const WireEvent& e) {
        if (e.kind == WireEvent::start) {
            c.initial = e.levels;
            started = true;
        } else if (e.kind == WireEvent::edge || e.kind == WireEvent::gap) {
            c.push(c.begin + e.tick, e.levels);
        }
    });
    if (!started) throw std::runtime_error("capture file: no data");
    c.done = true;
    return c;
}

void write_vcd(const Capture& c, std::ostream& out) {
    // Times in nanoseconds from the beginning (VCD has no negative times).
    const double ns_per_tick = 1e9 / c.tick_hz;
    const auto ns = [&](std::uint64_t tick) {
        return static_cast<std::uint64_t>(std::llround(static_cast<double>(tick - c.begin) * ns_per_tick));
    };
    static const char ids[kChannels] = {'!', '"', '#', '$'};
    out << "$date " << utc(c.started_ms, "{:%Y-%m-%d %H:%M:%S} UTC") << " $end\n";
    out << "$version forgecpp logic-analyzer $end\n";
    if (c.trigger) out << "$comment trigger at " << ns(*c.trigger) << " ns $end\n";
    out << "$timescale 1 ns $end\n$scope module logic $end\n";
    for (unsigned ch = 0; ch < kChannels; ++ch) {
        std::string name = c.names[ch].empty() ? std::format("D{}", ch) : c.names[ch];
        std::replace(name.begin(), name.end(), ' ', '_');
        out << "$var wire 1 " << ids[ch] << ' ' << name << " $end\n";
    }
    out << "$upscope $end\n$enddefinitions $end\n#0\n$dumpvars\n";
    for (unsigned ch = 0; ch < kChannels; ++ch) out << ((c.initial >> ch) & 1u) << ids[ch] << '\n';
    out << "$end\n";
    for (std::size_t i = 0; i < c.size(); ++i) {
        const std::uint8_t diff = c.before(i) ^ c.lv[i];
        out << '#' << ns(c.t[i]) << '\n';
        for (unsigned ch = 0; ch < kChannels; ++ch) {
            if ((diff >> ch) & 1u) out << ((c.lv[i] >> ch) & 1u) << ids[ch] << '\n';
        }
    }
    out << '#' << ns(c.end) << '\n';
}

void write_csv(const Capture& c, std::ostream& out) {
    out << "time_s";
    for (unsigned ch = 0; ch < kChannels; ++ch)
        out << ',' << (c.names[ch].empty() ? std::format("D{}", ch) : c.names[ch]);
    out << '\n';
    const auto row = [&](std::uint64_t tick, std::uint8_t lv) {
        out << std::format("{:.9f}", c.time_of(tick));
        for (unsigned ch = 0; ch < kChannels; ++ch) out << ',' << ((lv >> ch) & 1u);
        out << '\n';
    };
    row(c.begin, c.initial);
    for (std::size_t i = 0; i < c.size(); ++i) row(c.t[i], c.lv[i]);
}

// --- the store -------------------------------------------------------------------------

CaptureStore::CaptureStore(fs::path dir, std::size_t keep)
    : dir_(std::move(dir)), keep_(std::max<std::size_t>(1, keep)) {
    fs::create_directories(dir_);
}

std::string CaptureStore::save(const Capture& c, const json& extra) {
    std::lock_guard lock(mutex_);
    const std::string stem = std::format("capture-{}-{}", utc(c.started_ms, "{:%Y%m%d-%H%M%S}"), c.id);
    std::string name = stem + ".lacap";
    for (int n = 2; fs::exists(dir_ / name); ++n) name = std::format("{}-{}.lacap", stem, n);  // an import of the same
    const fs::path tmp = dir_ / (name + ".part");
    {
        std::ofstream out(tmp, std::ios::binary);
        write_capture(c, out, extra);
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
    }
    fs::rename(tmp, dir_ / name);
    prune();
    return name;
}

json CaptureStore::list() const {
    std::lock_guard lock(mutex_);
    std::vector<std::pair<fs::file_time_type, json>> items;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir_, ec)) {
        if (e.path().extension() != ".lacap") continue;
        std::ifstream in(e.path(), std::ios::binary);
        std::string magic, header;
        std::getline(in, magic);
        std::getline(in, header);
        json j;
        try {
            const json h = json::parse(header);
            const double hz = h.at("tick_hz").get<double>();
            j = {{"file", e.path().filename().string()},
                 {"bytes", e.file_size()},
                 {"id", h.value("id", 0)},
                 {"started_ms", h.value("started_ms", 0ll)},
                 {"started", utc(h.value("started_ms", 0ll), "{:%Y-%m-%d %H:%M:%S}")},
                 {"tick_hz", hz},
                 {"duration_s",
                  static_cast<double>(h.at("end").get<std::uint64_t>() - h.at("begin").get<std::uint64_t>()) / hz},
                 {"edges", h.value("edges", 0)},
                 {"trigger", !h.value("trigger", json(nullptr)).is_null()},
                 {"source", h.value("source", std::string())}};
        } catch (const std::exception&) {
            continue;  // not one of ours
        }
        items.emplace_back(e.last_write_time(), std::move(j));
    }
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    json out = json::array();
    for (auto& [t, j] : items) out.push_back(std::move(j));
    return out;
}

fs::path CaptureStore::path(const std::string& file) const {
    if (file.empty() || file.find('/') != std::string::npos || file.find("..") != std::string::npos ||
        !file.ends_with(".lacap"))
        throw std::invalid_argument("not a capture file name: " + file);
    return dir_ / file;
}

Capture CaptureStore::load(const std::string& file, json* extra) const {
    std::ifstream in(path(file), std::ios::binary);
    if (!in) throw std::invalid_argument("no capture " + file);
    return read_capture(in, extra);
}

bool CaptureStore::remove(const std::string& file) {
    std::lock_guard lock(mutex_);
    std::error_code ec;
    return fs::remove(path(file), ec);
}

void CaptureStore::prune() {
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir_, ec)) {
        if (e.path().extension() == ".lacap") files.emplace_back(e.last_write_time(), e.path());
    }
    if (files.size() <= keep_) return;
    std::sort(files.begin(), files.end());
    for (std::size_t i = 0; i + keep_ < files.size(); ++i) fs::remove(files[i].second, ec);
}

}  // namespace la
