#pragma once
// A capture: the changes of the four inputs, with their times.
//
// Stored as two arrays (structure of arrays): the tick of each change and the
// levels after it, 9 bytes per change. Four million changes are 36 MB: a few
// seconds of busy buses, or hours of a UART that is mostly idle.
//
// Ticks count from the start of the stream at tick_hz (the sample rate). A
// capture covers [begin, end]; begin moves forward while a trigger is awaited
// (only the pre-trigger time is kept).

#include "wire.hpp"

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace la {

struct Capture {
    std::uint64_t id = 0;
    std::int64_t started_ms = 0;  ///< wall clock (UTC ms) of the stream start
    double tick_hz = 100e6;       ///< sample rate
    std::uint64_t begin = 0;      ///< first tick covered
    std::uint64_t end = 0;        ///< last tick covered (grows while recording)
    std::uint8_t initial = 0;     ///< levels at `begin`
    std::vector<std::uint64_t> t; ///< ticks of the changes (ascending, > begin)
    std::vector<std::uint8_t> lv; ///< levels after each change
    std::optional<std::uint64_t> trigger;  ///< tick of the trigger, if one fired
    std::vector<std::pair<std::uint64_t, std::uint64_t>> gaps;  ///< data lost in [first, second]
    bool truncated = false;       ///< stopped at the edge limit
    bool done = false;            ///< no more changes will come
    std::array<std::string, kChannels> names{"D0", "D1", "D2", "D3"};
    std::string source;           ///< where it came from (sim:mixed, uio:/dev/uio0, file)

    std::size_t size() const { return t.size(); }
    /// Time 0 of the display: the trigger, else the beginning.
    std::uint64_t origin() const { return trigger.value_or(begin); }
    double seconds(std::uint64_t ticks) const { return static_cast<double>(ticks) / tick_hz; }
    /// Signed seconds from the origin.
    double time_of(std::uint64_t tick) const {
        return (static_cast<double>(tick) - static_cast<double>(origin())) / tick_hz;
    }

    /// Index of the first change at or after `tick`.
    std::size_t lower(std::uint64_t tick) const;
    /// Index of the first change after `tick`.
    std::size_t upper_index(std::uint64_t tick) const;
    /// Levels at `tick` (after the changes at that tick).
    std::uint8_t levels_at(std::uint64_t tick) const;
    bool level(unsigned ch, std::uint64_t tick) const { return (levels_at(tick) >> ch) & 1u; }
    /// Levels before change i.
    std::uint8_t before(std::size_t i) const { return i == 0 ? initial : lv[i - 1]; }

    /// Append a change (tick >= the last one); a change that changes nothing is ignored.
    void push(std::uint64_t tick, std::uint8_t levels);
    /// Drop everything before `tick` (pre-trigger buffer).
    void drop_before(std::uint64_t tick);
    /// Count of changes of one channel in [from, to].
    std::size_t count(unsigned ch, std::uint64_t from, std::uint64_t to) const;
};

/// Reads levels at increasing times without a binary search per call.
class Cursor {
public:
    explicit Cursor(const Capture& c, std::uint64_t from = 0) : c_(c), i_(c.lower(from)) {}
    /// Levels at `tick`; ticks must not decrease (else it searches again).
    std::uint8_t at(std::uint64_t tick) {
        if (i_ > 0 && i_ <= c_.t.size() && tick < c_.t[i_ - 1]) i_ = c_.lower(tick);
        while (i_ < c_.t.size() && c_.t[i_] <= tick) ++i_;
        return i_ == 0 ? c_.initial : c_.lv[i_ - 1];
    }
    /// Next change of channel `ch` strictly after `tick`, or nullopt.
    std::optional<std::uint64_t> next_edge(unsigned ch, std::uint64_t tick);

private:
    const Capture& c_;
    std::size_t i_;
};

// --- display ---------------------------------------------------------------------------

/// What one channel looks like between two times, at most a few items per pixel:
/// single edges where they can be told apart, "busy" spans where they cannot.
struct ChannelView {
    bool initial = false;  ///< level at `from`
    struct Item {
        std::uint64_t t0, t1;  ///< the edge (t0 == t1), or a busy span
        std::uint32_t count;   ///< edges in it (1 = a plain edge)
        bool level;            ///< level after it
    };
    std::vector<Item> items;
};

struct View {
    std::uint64_t from = 0, to = 0;
    std::array<ChannelView, kChannels> channels;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> gaps;
};

/// The view of [from, to] drawn `pixels` wide.
View make_view(const Capture& c, std::uint64_t from, std::uint64_t to, unsigned pixels);

// --- measurements ----------------------------------------------------------------------

struct ChannelStats {
    std::size_t rising = 0, falling = 0;
    double high_s = 0, low_s = 0;               ///< time spent high and low in the range
    std::optional<double> period_s;             ///< mean rising-to-rising
    std::optional<double> period_min_s, period_max_s;
    std::optional<double> width_high_min_s, width_high_max_s;  ///< complete high pulses
    std::optional<double> width_low_min_s, width_low_max_s;
    std::optional<double> frequency_hz() const {
        if (!period_s || *period_s <= 0) return std::nullopt;
        return 1.0 / *period_s;
    }
    std::optional<double> duty() const {
        const double total = high_s + low_s;
        if (total <= 0) return std::nullopt;
        return high_s / total;
    }
};

ChannelStats measure(const Capture& c, unsigned ch, std::uint64_t from, std::uint64_t to);

// --- sharing ---------------------------------------------------------------------------

/// A capture that one thread records while others read it.
class SharedCapture {
public:
    explicit SharedCapture(Capture c = {}) : c_(std::move(c)) {}
    template <class F>
    decltype(auto) read(F&& f) const {
        std::shared_lock lock(mutex_);
        return f(static_cast<const Capture&>(c_));
    }
    template <class F>
    decltype(auto) write(F&& f) {
        std::unique_lock lock(mutex_);
        return f(c_);
    }

private:
    mutable std::shared_mutex mutex_;
    Capture c_;
};

}  // namespace la
