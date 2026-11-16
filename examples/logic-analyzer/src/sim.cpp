#include "sim.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <stdexcept>
#include <thread>

namespace la {

using Edges = std::vector<std::pair<double, std::pair<unsigned, bool>>>;

const std::vector<double>& sample_rates() {
    static const std::vector<double> r{100e6, 50e6, 25e6, 20e6, 10e6, 5e6, 2e6, 1e6, 500e3, 200e3, 100e3};
    return r;
}

void uart_edges(double t0, double baud, const std::vector<std::uint8_t>& bytes, unsigned ch, Edges& out, unsigned bits,
                const std::string& parity, double stop) {
    const double T = 1.0 / baud;
    bool level = true;
    double t = t0;
    const auto set = [&](bool l) {
        if (l != level) out.push_back({t, {ch, l}});
        level = l;
    };
    for (const std::uint8_t b : bytes) {
        set(false);  // start bit
        t += T;
        unsigned ones = 0;
        for (unsigned k = 0; k < bits; ++k) {
            const bool bit = (b >> k) & 1u;
            ones += bit;
            set(bit);
            t += T;
        }
        if (parity != "none") {
            set(parity == "even" ? (ones & 1u) : !(ones & 1u));
            t += T;
        }
        set(true);
        t += stop * T;
    }
}

namespace {

// --- generators ------------------------------------------------------------------------

class UartGen : public Generator {
public:
    UartGen(unsigned ch, double baud, double period, double offset, std::size_t max_len,
            std::function<std::string(std::uint64_t)> message)
        : ch_(ch), baud_(baud), period_(period), offset_(offset), max_len_(max_len), message_(std::move(message)) {}
    double period_s() const override { return period_; }
    double offset_s() const override { return offset_; }
    double length_s() const override { return static_cast<double>(max_len_) * 10.0 / baud_; }
    void transaction(std::uint64_t k, Edges& out) const override {
        std::string m = message_(k);
        if (m.size() > max_len_) m.resize(max_len_);
        uart_edges(0, baud_, std::vector<std::uint8_t>(m.begin(), m.end()), ch_, out);
    }
    std::uint8_t idle_levels() const override { return static_cast<std::uint8_t>(1u << ch_); }
    std::uint8_t channel_mask() const override { return static_cast<std::uint8_t>(1u << ch_); }

private:
    unsigned ch_;
    double baud_, period_, offset_;
    std::size_t max_len_;
    std::function<std::string(std::uint64_t)> message_;
};

/// A sensor at 0x48 read with "write register 0, repeated start, read two
/// bytes"; every fifth time an absent device at 0x50 (NACK).
class I2cGen : public Generator {
public:
    I2cGen(unsigned scl, unsigned sda, double rate, double period, double offset)
        : scl_(scl), sda_(sda), q_(1.0 / (4 * rate)), period_(period), offset_(offset) {}
    double period_s() const override { return period_; }
    double offset_s() const override { return offset_; }
    double length_s() const override { return 50 * 4 * q_; }  // 4 bytes + starts and stop
    std::uint8_t idle_levels() const override { return static_cast<std::uint8_t>((1u << scl_) | (1u << sda_)); }
    std::uint8_t channel_mask() const override { return idle_levels(); }

    void transaction(std::uint64_t k, Edges& out) const override {
        double t = 0;
        bool scl = true, sda = true;
        const auto set_scl = [&](bool l) {
            if (l != scl) out.push_back({t, {scl_, l}});
            scl = l;
        };
        const auto set_sda = [&](bool l) {
            if (l != sda) out.push_back({t, {sda_, l}});
            sda = l;
        };
        const auto bit = [&](bool b) {
            t += q_;
            set_sda(b);
            t += q_;
            set_scl(true);
            t += 2 * q_;
            set_scl(false);
        };
        const auto byte = [&](std::uint8_t v, bool ack) {
            for (int i = 7; i >= 0; --i) bit((v >> i) & 1u);
            bit(!ack);
        };
        const auto start = [&] {
            if (!scl) {  // repeated start: release both first
                t += q_;
                set_sda(true);
                t += q_;
                set_scl(true);
            }
            t += q_;
            set_sda(false);
            t += q_;
            set_scl(false);
        };
        const auto stop = [&] {
            t += q_;
            set_sda(false);
            t += q_;
            set_scl(true);
            t += q_;
            set_sda(true);
        };
        if (k % 5 == 4) {
            start();
            byte(0x50 << 1, false);  // nobody there
            stop();
            return;
        }
        const double celsius = 24.0 + 3.0 * std::sin(static_cast<double>(k) / 40.0);
        const auto raw = static_cast<std::uint16_t>(std::lround(celsius * 256.0));
        start();
        byte(0x48 << 1, true);
        byte(0x00, true);
        start();
        byte((0x48 << 1) | 1, true);
        byte(static_cast<std::uint8_t>(raw >> 8), true);  // master ACKs
        byte(static_cast<std::uint8_t>(raw & 0xFF), false);  // and NACKs the last
        stop();
    }

private:
    unsigned scl_, sda_;
    double q_, period_, offset_;
};

/// A SPI flash in mode 0: read JEDEC ID, then read 4 data bytes, by turns.
class SpiGen : public Generator {
public:
    SpiGen(unsigned cs, unsigned sck, unsigned mosi, unsigned miso, double rate, double period)
        : cs_(cs), sck_(sck), mosi_(mosi), miso_(miso), h_(0.5 / rate), period_(period) {}
    double period_s() const override { return period_; }
    double offset_s() const override { return 0.0002; }
    double length_s() const override { return (8 * 8 * 2 + 4) * h_; }
    std::uint8_t idle_levels() const override { return static_cast<std::uint8_t>((1u << cs_) | (1u << miso_)); }
    std::uint8_t channel_mask() const override {
        return static_cast<std::uint8_t>((1u << cs_) | (1u << sck_) | (1u << mosi_) | (1u << miso_));
    }

    void transaction(std::uint64_t k, Edges& out) const override {
        std::vector<std::uint8_t> mo, mi;
        if (k % 2 == 0) {
            mo = {0x9F, 0x00, 0x00, 0x00};
            mi = {0xFF, 0xEF, 0x40, 0x18};
        } else {
            const std::uint32_t addr = static_cast<std::uint32_t>(k * 16) & 0xFFFFFF;
            mo = {0x03,
                  static_cast<std::uint8_t>(addr >> 16),
                  static_cast<std::uint8_t>(addr >> 8),
                  static_cast<std::uint8_t>(addr),
                  0,
                  0,
                  0,
                  0};
            mi = {0xFF, 0xFF, 0xFF, 0xFF};
            for (unsigned i = 0; i < 4; ++i) mi.push_back(static_cast<std::uint8_t>((addr + i) * 7 + 0x30));
        }
        double t = 0;
        std::uint8_t lv = idle_levels();
        const auto set = [&](unsigned ch, bool l) {
            if (((lv >> ch) & 1u) != static_cast<unsigned>(l)) out.push_back({t, {ch, l}});
            lv = l ? (lv | (1u << ch)) : (lv & ~(1u << ch));
        };
        set(cs_, false);
        for (std::size_t i = 0; i < mo.size(); ++i) {
            for (int b = 7; b >= 0; --b) {
                t += h_;
                set(mosi_, (mo[i] >> b) & 1u);  // both sides change data while SCK is low
                set(miso_, (mi[i] >> b) & 1u);
                t += h_;
                set(sck_, true);  // sampled here
                t += h_ / 2;
                set(sck_, false);
                t -= h_ / 2;
            }
        }
        t += 2 * h_;
        set(mosi_, false);
        set(miso_, true);
        set(cs_, true);
    }

private:
    unsigned cs_, sck_, mosi_, miso_;
    double h_, period_;
};

class PwmGen : public Generator {
public:
    PwmGen(unsigned ch, double freq, std::function<double(std::uint64_t)> duty, double offset = 0.00025)
        : ch_(ch), period_(1.0 / freq), offset_(offset), duty_(std::move(duty)) {}
    double period_s() const override { return period_; }
    double offset_s() const override { return offset_; }
    double length_s() const override { return period_; }
    std::uint8_t idle_levels() const override { return 0; }
    std::uint8_t channel_mask() const override { return static_cast<std::uint8_t>(1u << ch_); }
    void transaction(std::uint64_t k, Edges& out) const override {
        const double d = std::clamp(duty_(k), 0.001, 0.999);
        out.push_back({0, {ch_, true}});
        out.push_back({d * period_, {ch_, false}});
    }

private:
    unsigned ch_;
    double period_, offset_;
    std::function<double(std::uint64_t)> duty_;
};

}  // namespace

// --- scenarios -------------------------------------------------------------------------

std::uint8_t Scenario::initial() const {
    std::uint8_t lv = 0;
    for (const auto& g : generators) lv |= g->idle_levels();
    return lv;
}

void Scenario::edges(std::uint64_t from, std::uint64_t to, double tick_hz, std::vector<ChannelEdge>& out) const {
    if (to <= from) return;
    Edges tx;
    const double t_from = static_cast<double>(from) / tick_hz;
    const double t_to = static_cast<double>(to) / tick_hz;
    const std::size_t first = out.size();
    for (const auto& g : generators) {
        const double P = g->period_s(), L = g->length_s(), off = g->offset_s();
        if (t_to < off) continue;
        const double kmin_d = std::floor((t_from - off - L) / P);
        const std::uint64_t kmin = kmin_d < 0 ? 0 : static_cast<std::uint64_t>(kmin_d);
        const auto kmax = static_cast<std::uint64_t>(std::floor((t_to - off) / P));
        for (std::uint64_t k = kmin; k <= kmax; ++k) {
            tx.clear();
            g->transaction(k, tx);
            const double base = off + static_cast<double>(k) * P;
            for (const auto& [dt, e] : tx) {
                const auto tick = static_cast<std::uint64_t>(std::llround((base + dt) * tick_hz));
                if (tick > from && tick <= to) out.push_back({tick, static_cast<std::uint8_t>(e.first), e.second});
            }
        }
    }
    std::stable_sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end(),
                     [](const ChannelEdge& a, const ChannelEdge& b) { return a.tick < b.tick; });
}

const std::vector<std::string>& scenario_names() {
    static const std::vector<std::string> n{"mixed", "spi", "uart"};
    return n;
}

Scenario make_scenario(const std::string& name) {
    using forge::json;
    Scenario s;
    s.name = name;
    if (name == "mixed") {
        s.names = {"UART TX", "I2C SCL", "I2C SDA", "PWM"};
        s.generators.push_back(std::make_shared<UartGen>(0, 115200, 0.020, 0.001, 40, [](std::uint64_t k) {
            return std::format("Hello from Arty Z7! #{}\r\n", k);
        }));
        s.generators.push_back(std::make_shared<I2cGen>(1, 2, 100e3, 0.010, 0.004));
        s.generators.push_back(std::make_shared<PwmGen>(3, 1000, [](std::uint64_t k) {
            return 0.5 + 0.4 * std::sin(2 * std::numbers::pi * static_cast<double>(k) / 500.0);
        }));
        s.analyzers = json::array({
            {{"type", "uart"}, {"name", "UART"}, {"settings", {{"channel", 0}, {"baud", 115200}}}, {"radix", "ascii"}},
            {{"type", "i2c"}, {"name", "I2C"}, {"settings", {{"scl", 1}, {"sda", 2}}}, {"radix", "hex"}},
        });
    } else if (name == "spi") {
        s.names = {"CS", "SCK", "MOSI", "MISO"};
        s.generators.push_back(std::make_shared<SpiGen>(0, 1, 2, 3, 1e6, 0.005));
        s.analyzers = json::array({
            {{"type", "spi"},
             {"name", "SPI flash"},
             {"settings", {{"cs", 0}, {"sck", 1}, {"mosi", 2}, {"miso", 3}}},
             {"radix", "hex"}},
        });
    } else if (name == "uart") {
        s.names = {"TX", "RX", "CLK 10k", "Heartbeat"};
        s.generators.push_back(std::make_shared<UartGen>(0, 9600, 0.200, 0.002, 16, [](std::uint64_t k) {
            return k % 3 == 2 ? std::string("AT+VER?\r\n") : std::string("AT+TEMP?\r\n");
        }));
        s.generators.push_back(std::make_shared<UartGen>(1, 9600, 0.200, 0.020, 24, [](std::uint64_t k) {
            if (k % 3 == 2) return std::string("v1.4.2\r\nOK\r\n");
            const double c = 24.0 + 2.0 * std::sin(static_cast<double>(k) / 5.0);
            return std::format("+TEMP: {:.1f}\r\nOK\r\n", c);
        }));
        s.generators.push_back(std::make_shared<PwmGen>(2, 10000, [](std::uint64_t) { return 0.5; }, 0.00001));
        s.generators.push_back(std::make_shared<PwmGen>(3, 50, [](std::uint64_t) { return 0.05; }, 0.001));
        s.analyzers = json::array({
            {{"type", "uart"}, {"name", "TX"}, {"settings", {{"channel", 0}, {"baud", 9600}}}, {"radix", "ascii"}},
            {{"type", "uart"}, {"name", "RX"}, {"settings", {{"channel", 1}, {"baud", 9600}}}, {"radix", "ascii"}},
        });
    } else {
        throw std::invalid_argument("no simulator scenario \"" + name + "\" (mixed, spi, uart)");
    }
    return s;
}

// --- the source ------------------------------------------------------------------------

SimSource::SimSource(Scenario scenario, bool realtime, std::chrono::milliseconds step)
    : scenario_(std::move(scenario)), realtime_(realtime), step_(step) {}

void SimSource::start(const SourceConfig& config) {
    tick_hz_ = config.sample_hz;
    heartbeat_ = config.heartbeat_ticks ? config.heartbeat_ticks
                                        : std::max<std::uint64_t>(1, static_cast<std::uint64_t>(tick_hz_ / 1000));
    levels_ = scenario_.initial();
    done_ = 0;
    pending_until_ = 0;
    words_.clear();
    WireEncoder enc(words_);
    enc.start(levels_);
    t0_ = std::chrono::steady_clock::now();
    running_ = true;
}

void SimSource::stop() { running_ = false; }

void SimSource::generate(std::uint64_t to, std::vector<std::uint32_t>& out) {
    // done_: tick of the last word written; pending_until_: edges generated up to here.
    edges_.clear();
    scenario_.edges(pending_until_, to, tick_hz_, edges_);
    std::uint64_t tick = done_;
    const auto heartbeat_until = [&](std::uint64_t until) {
        while (until - tick > heartbeat_) {
            tick += heartbeat_;
            out.push_back(make_word(WordType::time, levels_, static_cast<std::uint32_t>(heartbeat_)));
        }
    };
    for (std::size_t i = 0; i < edges_.size();) {
        const std::uint64_t at = edges_[i].tick;
        std::uint8_t lv = levels_;
        for (; i < edges_.size() && edges_[i].tick == at; ++i) {
            const auto bit = static_cast<std::uint8_t>(1u << edges_[i].channel);
            lv = edges_[i].level ? (lv | bit) : (lv & ~bit);
        }
        if (lv == levels_) continue;
        heartbeat_until(at);
        out.push_back(make_word(WordType::edge, lv, static_cast<std::uint32_t>(at - tick)));
        tick = at;
        levels_ = lv;
    }
    heartbeat_until(to);
    if (to - tick == heartbeat_) {
        tick = to;
        out.push_back(make_word(WordType::time, levels_, static_cast<std::uint32_t>(heartbeat_)));
    }
    // Whatever is left (< a heartbeat) is carried by the next word's delta.
    done_ = tick;
    pending_until_ = to;
}

std::size_t SimSource::read(std::vector<std::uint32_t>& out, std::chrono::milliseconds timeout) {
    if (!running_) return 0;
    const std::size_t before = out.size();
    out.insert(out.end(), words_.begin(), words_.end());  // the start word
    words_.clear();
    std::uint64_t target;
    if (realtime_) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
            target = static_cast<std::uint64_t>(elapsed * tick_hz_);
            if (target > pending_until_ + heartbeat_ || std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // After a long stall do not make more than a quarter second at once.
        target = std::min(target, pending_until_ + static_cast<std::uint64_t>(0.25 * tick_hz_));
    } else {
        target = pending_until_ + static_cast<std::uint64_t>(std::chrono::duration<double>(step_).count() * tick_hz_);
    }
    if (target > pending_until_) generate(target, out);
    stats_.words += out.size() - before;
    return out.size() - before;
}

forge::json SimSource::info() const {
    return {{"scenario", scenario_.name}, {"signals", scenario_.names}, {"clock_hz", base_clock_hz()}};
}

std::vector<std::string> SimSource::channel_names() const { return {scenario_.names.begin(), scenario_.names.end()}; }

}  // namespace la
