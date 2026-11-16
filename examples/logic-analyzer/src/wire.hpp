#pragma once
// The words the FPGA writes into the capture buffer (fpga/README.md).
//
// The FPGA does not send samples: it sends changes. Each 32-bit word says
// how many sample ticks passed since the previous word and what the four
// inputs read after that time. A line that does not move costs nothing but
// a heartbeat word now and then; a UART at 115200 baud costs about 100 000
// words per second instead of 100 million samples.
//
//   31 30 | 29 28 27 26 | 25 ............................ 0
//   type  | levels 3..0 | delta: ticks since the previous word
//
//   type 0  time    no change (heartbeat, or a delta too long for 26 bits)
//   type 1  edge    one or more inputs changed; levels are the new ones
//   type 2  gap     words were lost (the buffer was full) before this one;
//                   delta still counts the time, levels are the current ones
//   type 3  start   first word of a capture: the levels at the start, delta 0
//
// Little endian in memory. 26 bits of delta are 0.67 s at 100 MHz; the FPGA
// sends a time word before the delta would overflow.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace la {

constexpr unsigned kChannels = 4;
constexpr std::uint32_t kDeltaBits = 26;
constexpr std::uint32_t kMaxDelta = (std::uint32_t{1} << kDeltaBits) - 1;

enum class WordType : std::uint8_t { time = 0, edge = 1, gap = 2, start = 3 };

constexpr std::uint32_t make_word(WordType type, std::uint8_t levels, std::uint32_t delta) {
    return (static_cast<std::uint32_t>(type) << 30) | (static_cast<std::uint32_t>(levels & 0xF) << 26) |
           (delta & kMaxDelta);
}
constexpr WordType word_type(std::uint32_t w) { return static_cast<WordType>(w >> 30); }
constexpr std::uint8_t word_levels(std::uint32_t w) { return static_cast<std::uint8_t>((w >> 26) & 0xF); }
constexpr std::uint32_t word_delta(std::uint32_t w) { return w & kMaxDelta; }

/// What the decoder hands on: a change of the inputs, a gap, or just time.
struct WireEvent {
    enum Kind : std::uint8_t { start, edge, gap, time } kind;
    std::uint64_t tick;    ///< ticks since the start word
    std::uint8_t levels;   ///< after the event
};

/// Turns words into events with absolute times. Feed it words in order;
/// it may be fed in pieces of any size.
class WireDecoder {
public:
    /// Words before the first start word are ignored (counted in skipped()).
    template <class Out>  // Out: void(const WireEvent&)
    void feed(std::span<const std::uint32_t> words, Out&& out) {
        for (const std::uint32_t w : words) {
            const auto type = word_type(w);
            if (type == WordType::start) {
                started_ = true;
                tick_ = 0;
                levels_ = word_levels(w);
                out(WireEvent{WireEvent::start, 0, levels_});
                continue;
            }
            if (!started_) {
                ++skipped_;
                continue;
            }
            tick_ += word_delta(w);
            const std::uint8_t lv = word_levels(w);
            switch (type) {
                case WordType::edge:
                    if (lv != levels_) {  // an edge word that changes nothing is only time
                        levels_ = lv;
                        out(WireEvent{WireEvent::edge, tick_, levels_});
                    } else {
                        out(WireEvent{WireEvent::time, tick_, levels_});
                    }
                    break;
                case WordType::gap:
                    levels_ = lv;
                    ++gaps_;
                    out(WireEvent{WireEvent::gap, tick_, levels_});
                    break;
                default: out(WireEvent{WireEvent::time, tick_, levels_}); break;
            }
        }
    }

    bool started() const { return started_; }
    std::uint64_t tick() const { return tick_; }
    std::uint8_t levels() const { return levels_; }
    std::uint64_t skipped() const { return skipped_; }
    std::uint64_t gaps() const { return gaps_; }

private:
    bool started_ = false;
    std::uint64_t tick_ = 0;
    std::uint8_t levels_ = 0;
    std::uint64_t skipped_ = 0, gaps_ = 0;
};

/// The other direction, as the FPGA does it (the simulator, tests, capture files).
class WireEncoder {
public:
    explicit WireEncoder(std::vector<std::uint32_t>& out) : out_(out) {}

    void start(std::uint8_t levels) {
        out_.push_back(make_word(WordType::start, levels, 0));
        tick_ = 0;
        levels_ = levels & 0xF;
    }
    /// The inputs read `levels` from `tick` on (tick >= the previous one).
    void change(std::uint64_t tick, std::uint8_t levels) {
        levels &= 0xF;
        advance(tick);
        if (levels == levels_) return;
        out_.push_back(make_word(WordType::edge, levels, static_cast<std::uint32_t>(tick - tick_)));
        tick_ = tick;
        levels_ = levels;
    }
    /// Time passes without a change (a heartbeat at `tick`).
    void time(std::uint64_t tick) {
        advance(tick);
        if (tick == tick_) return;
        out_.push_back(make_word(WordType::time, levels_, static_cast<std::uint32_t>(tick - tick_)));
        tick_ = tick;
    }
    /// Words were lost; at `tick` the inputs read `levels`.
    void gap(std::uint64_t tick, std::uint8_t levels) {
        advance(tick);
        out_.push_back(make_word(WordType::gap, levels, static_cast<std::uint32_t>(tick - tick_)));
        tick_ = tick;
        levels_ = levels & 0xF;
    }
    std::uint64_t tick() const { return tick_; }
    std::uint8_t levels() const { return levels_; }

private:
    // Time words until the rest of the way fits in one delta.
    void advance(std::uint64_t tick) {
        while (tick - tick_ > kMaxDelta) {
            out_.push_back(make_word(WordType::time, levels_, kMaxDelta));
            tick_ += kMaxDelta;
        }
    }

    std::vector<std::uint32_t>& out_;
    std::uint64_t tick_ = 0;
    std::uint8_t levels_ = 0;
};

}  // namespace la
