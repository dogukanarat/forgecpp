#include "capture.hpp"

#include <algorithm>
#include <limits>

namespace la {

std::size_t Capture::lower(std::uint64_t tick) const {
    return static_cast<std::size_t>(std::lower_bound(t.begin(), t.end(), tick) - t.begin());
}

std::size_t Capture::upper_index(std::uint64_t tick) const {
    return static_cast<std::size_t>(std::upper_bound(t.begin(), t.end(), tick) - t.begin());
}

std::uint8_t Capture::levels_at(std::uint64_t tick) const {
    const auto i = static_cast<std::size_t>(std::upper_bound(t.begin(), t.end(), tick) - t.begin());
    return i == 0 ? initial : lv[i - 1];
}

void Capture::push(std::uint64_t tick, std::uint8_t levels) {
    levels &= 0xF;
    const std::uint8_t now = t.empty() ? initial : lv.back();
    if (levels == now) return;
    if (!t.empty() && tick == t.back()) {  // two words at one tick: keep the last state
        lv.back() = levels;
        if (lv.size() >= 2 ? lv[lv.size() - 2] == levels : initial == levels) {
            t.pop_back();
            lv.pop_back();
        }
        return;
    }
    if (tick <= begin && t.empty()) {  // at the very beginning: it is the initial state
        initial = levels;
        return;
    }
    t.push_back(tick);
    lv.push_back(levels);
}

void Capture::drop_before(std::uint64_t tick) {
    if (tick <= begin) return;
    const std::size_t n = static_cast<std::size_t>(std::upper_bound(t.begin(), t.end(), tick) - t.begin());
    if (n > 0) {
        initial = lv[n - 1];
        t.erase(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(n));
        lv.erase(lv.begin(), lv.begin() + static_cast<std::ptrdiff_t>(n));
    }
    begin = tick;
    std::erase_if(gaps, [&](const auto& g) { return g.second < tick; });
}

std::size_t Capture::count(unsigned ch, std::uint64_t from, std::uint64_t to) const {
    std::size_t n = 0;
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << ch);
    for (std::size_t i = lower(from); i < t.size() && t[i] <= to; ++i) {
        if ((before(i) ^ lv[i]) & bit) ++n;
    }
    return n;
}

std::optional<std::uint64_t> Cursor::next_edge(unsigned ch, std::uint64_t tick) {
    at(tick);  // i_: first change after tick
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << ch);
    for (; i_ < c_.t.size(); ++i_) {
        if ((c_.before(i_) ^ c_.lv[i_]) & bit) return c_.t[i_];
    }
    return std::nullopt;
}

View make_view(const Capture& c, std::uint64_t from, std::uint64_t to, unsigned pixels) {
    View v;
    if (to < from) std::swap(from, to);
    v.from = from;
    v.to = to;
    pixels = std::max(1u, pixels);
    const double span = static_cast<double>(to - from) + 1.0;
    const auto bucket = [&](std::uint64_t tick) {
        return static_cast<std::int64_t>(static_cast<double>(tick - from) * pixels / span);
    };
    const std::uint8_t start = c.levels_at(from);
    for (unsigned ch = 0; ch < kChannels; ++ch) v.channels[ch].initial = (start >> ch) & 1u;

    std::uint8_t prev = start;
    for (std::size_t i = c.upper_index(from); i < c.t.size() && c.t[i] <= to; ++i) {
        const std::uint8_t now = c.lv[i];
        const std::uint8_t diff = prev ^ now;
        prev = now;
        if (!diff) continue;
        const std::uint64_t tick = c.t[i];
        const auto b = bucket(tick);
        for (unsigned ch = 0; ch < kChannels; ++ch) {
            if (!((diff >> ch) & 1u)) continue;
            const bool level = (now >> ch) & 1u;
            auto& items = v.channels[ch].items;
            if (!items.empty()) {
                auto& last = items.back();
                const auto lb = bucket(last.t1);
                if (b == lb || (last.count > 1 && b == lb + 1)) {  // too close to tell apart
                    last.t1 = tick;
                    ++last.count;
                    last.level = level;
                    continue;
                }
            }
            items.push_back({tick, tick, 1, level});
        }
    }
    for (const auto& g : c.gaps) {
        if (g.second >= from && g.first <= to) v.gaps.push_back(g);
    }
    return v;
}

ChannelStats measure(const Capture& c, unsigned ch, std::uint64_t from, std::uint64_t to) {
    ChannelStats s;
    if (to <= from) return s;
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << ch);
    const std::size_t first = c.lower(from);  // edges at `from` count
    bool level = (c.before(first) >> ch) & 1u;
    std::uint64_t last = from;
    std::optional<std::uint64_t> last_rise, last_fall;
    double period_sum = 0;
    std::size_t periods = 0;
    const auto upd_min = [](std::optional<double>& m, double v) {
        if (!m || v < *m) m = v;
    };
    const auto upd_max = [](std::optional<double>& m, double v) {
        if (!m || v > *m) m = v;
    };
    for (std::size_t i = first; i < c.t.size() && c.t[i] <= to; ++i) {
        if (!((c.before(i) ^ c.lv[i]) & bit)) continue;
        const std::uint64_t tick = c.t[i];
        const double dt = c.seconds(tick - last);
        (level ? s.high_s : s.low_s) += dt;
        level = !level;
        last = tick;
        if (level) {  // rising
            ++s.rising;
            if (last_rise) {
                const double p = c.seconds(tick - *last_rise);
                period_sum += p;
                ++periods;
                upd_min(s.period_min_s, p);
                upd_max(s.period_max_s, p);
            }
            if (last_fall) {
                const double w = c.seconds(tick - *last_fall);
                upd_min(s.width_low_min_s, w);
                upd_max(s.width_low_max_s, w);
            }
            last_rise = tick;
        } else {
            ++s.falling;
            if (last_rise) {
                const double w = c.seconds(tick - *last_rise);
                upd_min(s.width_high_min_s, w);
                upd_max(s.width_high_max_s, w);
            }
            last_fall = tick;
        }
    }
    (level ? s.high_s : s.low_s) += c.seconds(to - last);
    if (periods > 0) s.period_s = period_sum / static_cast<double>(periods);
    return s;
}

}  // namespace la
