#include "decoders.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <stdexcept>

namespace la {

using forge::json;

namespace {

json field_channel(const char* key, const char* label, int def, bool optional = false) {
    return {{"key", key}, {"label", label}, {"kind", optional ? "channel_opt" : "channel"}, {"default", def}};
}
json field_int(const char* key, const char* label, std::int64_t def, std::int64_t min, std::int64_t max,
               const char* unit = "") {
    return {{"key", key}, {"label", label}, {"kind", "int"}, {"default", def},
            {"min", min}, {"max", max},     {"unit", unit}};
}
json field_choice(const char* key, const char* label, json def, json choices) {
    return {{"key", key}, {"label", label}, {"kind", "choice"}, {"default", def}, {"choices", choices}};
}
json field_bool(const char* key, const char* label, bool def) {
    return {{"key", key}, {"label", label}, {"kind", "bool"}, {"default", def}};
}

const json& find_type(const std::string& type) {
    for (const auto& t : decoder_types()) {
        if (t["type"] == type) return t;
    }
    throw std::invalid_argument("no decoder \"" + type + "\" (uart, i2c, spi)");
}

// --- UART ------------------------------------------------------------------------------

class Uart : public Decoder {
public:
    explicit Uart(const json& s)
        : ch_(s["channel"].get<unsigned>()),
          baud_(s["baud"].get<double>()),
          bits_(s["bits"].get<unsigned>()),
          parity_(s["parity"].get<std::string>()),
          stop_(std::stod(s["stop"].get<std::string>())),
          inverted_(s["inverted"].get<bool>()),
          msb_first_(s["msb_first"].get<bool>()) {}

    unsigned display_channel() const override { return ch_; }

    std::vector<DecodedFrame> decode(const Capture& c) const override {
        std::vector<DecodedFrame> out;
        const double T = c.tick_hz / baud_;
        if (T < 2) return out;  // fewer than two samples per bit: nothing to read
        const unsigned par = parity_ == "none" ? 0 : 1;
        const auto at = [&](std::uint64_t ts, double bit) {
            return ts + static_cast<std::uint64_t>(std::llround(bit * T));
        };
        Cursor cur(c, c.begin);
        const auto logical = [&](std::uint64_t tick) {
            const bool l = (cur.at(tick) >> ch_) & 1u;
            return inverted_ ? !l : l;
        };
        std::uint64_t pos = c.begin;
        for (;;) {
            const auto e = cur.next_edge(ch_, pos);
            if (!e) break;
            pos = *e;
            if (logical(*e)) continue;  // a rising edge: not a start bit
            const std::uint64_t ts = *e;
            const std::uint64_t stop_sample = at(ts, 1.5 + bits_ + par);
            if (stop_sample > c.end) break;  // the capture ends inside this frame
            std::uint32_t value = 0;
            unsigned ones = 0;
            for (unsigned k = 0; k < bits_; ++k) {
                const bool b = logical(at(ts, 1.5 + k));
                ones += b;
                if (msb_first_) {
                    value = (value << 1) | (b ? 1u : 0u);
                } else if (b) {
                    value |= 1u << k;
                }
            }
            DecodedFrame f;
            f.start = ts;
            f.end = std::min(c.end, at(ts, 1.0 + bits_ + par + stop_));
            f.value = value;
            f.bits = static_cast<std::uint8_t>(bits_);
            bool parity_bit = false;
            if (par) {
                parity_bit = logical(at(ts, 1.5 + bits_));
                const bool odd = ((ones + (parity_bit ? 1u : 0u)) & 1u) != 0;
                if ((parity_ == "even" && odd) || (parity_ == "odd" && !odd)) f.flags |= kParityError;
            }
            if (!logical(stop_sample)) {
                f.flags |= kFramingError;
                if (value == 0 && !parity_bit) f.flags |= kBreak;
            }
            out.push_back(f);
            pos = stop_sample;  // the next start bit comes after the middle of the stop bit
        }
        return out;
    }

private:
    unsigned ch_;
    double baud_;
    unsigned bits_;
    std::string parity_;
    double stop_;
    bool inverted_, msb_first_;
};

// --- I2C -------------------------------------------------------------------------------

class I2c : public Decoder {
public:
    explicit I2c(const json& s) : scl_(s["scl"].get<unsigned>()), sda_(s["sda"].get<unsigned>()) {
        if (scl_ == sda_) throw std::invalid_argument("SCL and SDA must be different channels");
    }
    unsigned display_channel() const override { return sda_; }

    std::vector<DecodedFrame> decode(const Capture& c) const override {
        std::vector<DecodedFrame> out;
        bool in = false, address_next = false;
        unsigned count = 0;
        std::uint32_t byte = 0;
        std::uint64_t byte_start = c.begin, last_fall = c.begin;
        bool scl = c.initial >> scl_ & 1u, sda = c.initial >> sda_ & 1u;

        // START and STOP conditions come after a rising SCL that looked like a
        // first data bit; only a byte cut after more than that is an error.
        const auto partial = [&](std::uint64_t t) {
            if (in && count > 1) {
                DecodedFrame f{byte_start, t, FrameKind::error, kIncomplete, byte, 0, static_cast<std::uint8_t>(count)};
                out.push_back(f);
            }
            count = 0;
            byte = 0;
        };
        const auto on_sda = [&](std::uint64_t t, bool level) {
            sda = level;
            if (!scl) return;
            if (!level) {  // falling while SCL high: START
                const bool repeated = in;
                partial(t);
                out.push_back({t, t, FrameKind::start, static_cast<std::uint16_t>(repeated ? kRepeated : 0)});
                in = true;
                address_next = true;
                byte_start = t;
            } else if (in) {  // rising while SCL high: STOP
                partial(t);
                out.push_back({t, t, FrameKind::stop, 0});
                in = false;
            }
        };
        const auto on_scl = [&](std::uint64_t t, bool level) {
            scl = level;
            if (!level) {
                last_fall = t;
                if (count == 0) byte_start = t;
                return;
            }
            if (!in) return;
            if (count < 8) {
                if (count == 0 && byte_start < last_fall) byte_start = last_fall;
                byte = (byte << 1) | (sda ? 1u : 0u);
                ++count;
                return;
            }
            // the 9th clock: ACK (SDA low) or NACK
            DecodedFrame f;
            f.start = byte_start;
            f.end = t + (t > last_fall ? t - last_fall : 0);
            f.flags = sda ? kNack : 0;
            if (address_next) {
                f.kind = FrameKind::address;
                f.value = byte >> 1;
                f.value2 = byte;
                f.bits = 7;
                if (byte & 1u) f.flags |= kRead;
                address_next = false;
            } else {
                f.kind = FrameKind::data;
                f.value = byte;
            }
            out.push_back(f);
            count = 0;
            byte = 0;
        };

        for (std::size_t i = 0; i < c.t.size(); ++i) {
            const std::uint64_t t = c.t[i];
            const std::uint8_t b = c.before(i), a = c.lv[i];
            const bool scl_changed = ((b ^ a) >> scl_) & 1u;
            const bool sda_changed = ((b ^ a) >> sda_) & 1u;
            const bool scl_new = (a >> scl_) & 1u, sda_new = (a >> sda_) & 1u;
            if (scl_changed && sda_changed) {
                // At one tick (a slow sample rate): data changes while SCL is low.
                if (scl_new) {
                    on_sda(t, sda_new);
                    on_scl(t, true);
                } else {
                    on_scl(t, false);
                    on_sda(t, sda_new);
                }
            } else if (scl_changed) {
                on_scl(t, scl_new);
            } else if (sda_changed) {
                on_sda(t, sda_new);
            }
        }
        return out;
    }

private:
    unsigned scl_, sda_;
};

// --- SPI -------------------------------------------------------------------------------

class Spi : public Decoder {
public:
    explicit Spi(const json& s)
        : cs_(s["cs"].get<int>()),
          sck_(s["sck"].get<unsigned>()),
          mosi_(s["mosi"].get<int>()),
          miso_(s["miso"].get<int>()),
          cpol_(s["cpol"].get<std::string>() == "1"),
          cpha_(s["cpha"].get<std::string>() == "1"),
          bits_(s["bits"].get<unsigned>()),
          msb_first_(s["msb_first"].get<bool>()),
          cs_active_low_(s["cs_active_low"].get<bool>()) {
        if (mosi_ < 0 && miso_ < 0) throw std::invalid_argument("SPI needs MOSI or MISO");
        for (int ch : {cs_, mosi_, miso_}) {
            if (ch == static_cast<int>(sck_)) throw std::invalid_argument("SCK must have a channel of its own");
        }
    }
    unsigned display_channel() const override { return static_cast<unsigned>(mosi_ >= 0 ? mosi_ : miso_); }

    std::vector<DecodedFrame> decode(const Capture& c) const override {
        std::vector<DecodedFrame> out;
        const bool sample_rising = cpol_ == cpha_;  // modes 0 and 3
        const auto active_of = [&](std::uint8_t lv) {
            if (cs_ < 0) return true;
            const bool l = (lv >> cs_) & 1u;
            return cs_active_low_ ? !l : l;
        };
        bool active = active_of(c.initial);
        unsigned count = 0;
        std::uint32_t mosi = 0, miso = 0;
        std::uint64_t word_start = c.begin, prev_end = c.begin, cs_time = c.begin;
        std::optional<std::uint64_t> last_other, last_sample;
        std::uint64_t last_interval = 0;
        const std::uint16_t lines = static_cast<std::uint16_t>((mosi_ < 0 ? kNoMosi : 0) | (miso_ < 0 ? kNoMiso : 0));

        const auto cut = [&](std::uint64_t t) {
            if (count > 0) {
                out.push_back({word_start, t, FrameKind::error, static_cast<std::uint16_t>(kIncomplete | lines), mosi,
                               miso, static_cast<std::uint8_t>(count)});
            }
            count = 0;
            mosi = miso = 0;
        };

        for (std::size_t i = 0; i < c.t.size(); ++i) {
            const std::uint64_t t = c.t[i];
            const std::uint8_t b = c.before(i), a = c.lv[i];
            const bool now_active = active_of(a);
            if (now_active != active) {
                active = now_active;
                if (active) {
                    count = 0;
                    mosi = miso = 0;
                    cs_time = t;
                    last_other.reset();
                } else {
                    cut(t);
                }
            }
            if (!active || !(((b ^ a) >> sck_) & 1u)) continue;
            const bool rising = (a >> sck_) & 1u;
            if (rising != sample_rising) {
                last_other = t;
                continue;
            }
            // A sampling edge. Without CS, a pause of many clocks starts a new word.
            if (cs_ < 0 && last_sample && last_interval > 0 && t - *last_sample > 8 * last_interval) cut(t);
            if (last_sample) last_interval = t - *last_sample;
            last_sample = t;
            if (count == 0) {
                const std::uint64_t floor = std::max(cs_time, prev_end);
                word_start = (last_other && *last_other >= floor) ? *last_other : floor;
                if (cs_ < 0 && !(last_other && *last_other >= floor)) word_start = t;
            }
            // Data lines are read as they were just before the clock edge.
            const bool mo = mosi_ >= 0 && ((b >> mosi_) & 1u);
            const bool mi = miso_ >= 0 && ((b >> miso_) & 1u);
            if (msb_first_) {
                mosi = (mosi << 1) | (mo ? 1u : 0u);
                miso = (miso << 1) | (mi ? 1u : 0u);
            } else {
                mosi |= (mo ? 1u : 0u) << count;
                miso |= (mi ? 1u : 0u) << count;
            }
            if (++count == bits_) {
                const std::uint64_t half = last_other && *last_other < t ? t - *last_other : 0;
                out.push_back(
                    {word_start, t + half, FrameKind::data, lines, mosi, miso, static_cast<std::uint8_t>(bits_)});
                prev_end = t;
                count = 0;
                mosi = miso = 0;
            }
        }
        return out;
    }

private:
    int cs_;
    unsigned sck_;
    int mosi_, miso_;
    bool cpol_, cpha_;
    unsigned bits_;
    bool msb_first_, cs_active_low_;
};

}  // namespace

const json& decoder_types() {
    static const json types = json::array({
        {{"type", "uart"},
         {"label", "Async serial (UART)"},
         {"fields",
          json::array(
              {field_channel("channel", "Channel", 0), field_int("baud", "Bit rate", 115200, 50, 20000000, "bit/s"),
               field_int("bits", "Data bits", 8, 5, 9),
               field_choice("parity", "Parity", "none", {"none", "even", "odd"}),
               field_choice("stop", "Stop bits", "1", {"1", "1.5", "2"}),
               field_bool("inverted", "Inverted (idle low)", false), field_bool("msb_first", "MSB first", false)})}},
        {{"type", "i2c"},
         {"label", "I2C"},
         {"fields", json::array({field_channel("scl", "SCL", 1), field_channel("sda", "SDA", 2)})}},
        {{"type", "spi"},
         {"label", "SPI"},
         {"fields",
          json::array({field_channel("cs", "Enable (CS)", 0, true), field_channel("sck", "Clock (SCK)", 1),
                       field_channel("mosi", "MOSI", 2, true), field_channel("miso", "MISO", 3, true),
                       field_choice("cpol", "Clock polarity (CPOL)", "0", {"0", "1"}),
                       field_choice("cpha", "Clock phase (CPHA)", "0", {"0", "1"}),
                       field_int("bits", "Bits per word", 8, 4, 32), field_bool("msb_first", "MSB first", true),
                       field_bool("cs_active_low", "CS active low", true)})}},
    });
    return types;
}

json normalize_settings(const std::string& type, const json& settings) {
    const json& t = find_type(type);
    if (!settings.is_null() && !settings.is_object()) throw std::invalid_argument("settings must be an object");
    json out = json::object();
    for (const auto& f : t["fields"]) {
        const std::string key = f["key"];
        const std::string kind = f["kind"];
        const json v = settings.is_object() && settings.contains(key) ? settings[key] : f["default"];
        const auto bad = [&](const std::string& why) {
            return std::invalid_argument(std::format("{}: {}", f["label"].get<std::string>(), why));
        };
        if (kind == "channel" || kind == "channel_opt") {
            if (!v.is_number_integer()) throw bad("a channel number");
            const int ch = v.get<int>();
            if (ch < (kind == "channel" ? 0 : -1) || ch >= static_cast<int>(kChannels))
                throw bad(kind == "channel" ? "channel 0 to 3" : "channel 0 to 3, or -1 for none");
            out[key] = ch;
        } else if (kind == "int") {
            if (!v.is_number()) throw bad("a number");
            const double d = v.get<double>();
            if (d != std::floor(d) || d < f["min"].get<double>() || d > f["max"].get<double>())
                throw bad(std::format("a whole number from {} to {}", f["min"].dump(), f["max"].dump()));
            out[key] = static_cast<std::int64_t>(d);
        } else if (kind == "choice") {
            const std::string s = v.is_string() ? v.get<std::string>() : v.dump();
            if (std::find(f["choices"].begin(), f["choices"].end(), json(s)) == f["choices"].end())
                throw bad("one of " + f["choices"].dump());
            out[key] = s;
        } else if (kind == "bool") {
            if (!v.is_boolean()) throw bad("true or false");
            out[key] = v;
        }
    }
    make_decoder(type, out);  // channel conflicts
    return out;
}

std::unique_ptr<Decoder> make_decoder(const std::string& type, const json& s) {
    if (type == "uart") return std::make_unique<Uart>(s);
    if (type == "i2c") return std::make_unique<I2c>(s);
    if (type == "spi") return std::make_unique<Spi>(s);
    throw std::invalid_argument("no decoder \"" + type + "\"");
}

std::string format_value(std::uint32_t value, unsigned bits, const std::string& radix) {
    if (radix == "dec") return std::to_string(value);
    if (radix == "bin") {
        std::string s = "0b";
        for (int i = static_cast<int>(bits) - 1; i >= 0; --i) s += ((value >> i) & 1u) ? '1' : '0';
        return s;
    }
    if (radix == "ascii") {
        if (value >= 0x20 && value < 0x7F) {
            if (value == '\'') return "'\\''";
            return std::format("'{}'", static_cast<char>(value));
        }
        switch (value) {
            case '\r': return "'\\r'";
            case '\n': return "'\\n'";
            case '\t': return "'\\t'";
            case 0: return "'\\0'";
            default: break;
        }
        return std::format("0x{:02X}", value);
    }
    const unsigned digits = std::max(2u, (bits + 3) / 4);
    return std::format("0x{:0{}X}", value, digits);
}

std::string describe(const std::string& type, const DecodedFrame& f, const std::string& radix) {
    std::string s;
    switch (f.kind) {
        case FrameKind::start: return (f.flags & kRepeated) ? "Repeated start" : "Start";
        case FrameKind::stop: return "Stop";
        case FrameKind::address:
            s = std::format("{} 0x{:02X}", (f.flags & kRead) ? "Read from" : "Write to", f.value);
            s += (f.flags & kNack) ? " NACK" : " ACK";
            return s;
        case FrameKind::error: s = std::format("Incomplete ({} bits)", f.bits); return s;
        case FrameKind::data: break;
    }
    if (type == "spi") {
        if (!(f.flags & kNoMosi)) s += "MOSI " + format_value(f.value, f.bits, radix);
        if (!(f.flags & kNoMiso))
            s += std::string(s.empty() ? "" : "  ") + "MISO " + format_value(f.value2, f.bits, radix);
        return s;
    }
    s = format_value(f.value, f.bits, radix);
    if (type == "i2c") s += (f.flags & kNack) ? " NACK" : " ACK";
    if (f.flags & kBreak) {
        s = "Break";
    } else {
        if (f.flags & kFramingError) s += " framing error";
        if (f.flags & kParityError) s += " parity error";
    }
    return s;
}

const char* kind_name(FrameKind k) {
    switch (k) {
        case FrameKind::data: return "data";
        case FrameKind::start: return "start";
        case FrameKind::stop: return "stop";
        case FrameKind::address: return "address";
        case FrameKind::error: return "error";
    }
    return "data";
}

}  // namespace la
