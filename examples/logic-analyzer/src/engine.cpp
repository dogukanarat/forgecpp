#include "engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace la {

const char* state_name(State s) {
    switch (s) {
        case State::idle: return "idle";
        case State::armed: return "armed";
        case State::recording: return "recording";
    }
    return "idle";
}

Engine::Engine(std::unique_ptr<Source> source, OnDone on_done)
    : source_(std::move(source)), on_done_(std::move(on_done)), thread_([this] { run(); }) {}

Engine::~Engine() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void Engine::start(const CaptureSettings& settings) {
    {
        std::lock_guard lock(mutex_);
        settings_ = settings;
        start_ = true;
        stop_ = true;  // ends a capture that is running; the thread then starts the new one
    }
    cv_.notify_all();
}

void Engine::stop() {
    {
        std::lock_guard lock(mutex_);
        start_ = false;
        settings_.repeat = false;
        stop_ = true;
    }
    cv_.notify_all();
}

std::shared_ptr<SharedCapture> Engine::current() const {
    std::lock_guard lock(mutex_);
    return current_;
}

EngineStatus Engine::status() const {
    std::lock_guard lock(mutex_);
    auto s = status_;
    s.source = source_->stats();
    return s;
}

void Engine::set_names(const std::array<std::string, kChannels>& names) {
    std::lock_guard lock(mutex_);
    names_ = names;
}

void Engine::run() {
    std::unique_lock lock(mutex_);
    for (;;) {
        cv_.wait(lock, [this] { return quit_ || start_; });
        if (quit_) return;
        start_ = false;
        stop_ = false;
        const CaptureSettings s = settings_;
        lock.unlock();
        capture(s);
        lock.lock();
        if (settings_.repeat && !start_ && !quit_ && status_.error.empty()) start_ = true;  // arm again
    }
}

void Engine::capture(const CaptureSettings& s) {
    using clock = std::chrono::steady_clock;
    auto shared = std::make_shared<SharedCapture>();
    std::array<std::string, kChannels> names;
    {
        std::lock_guard lock(mutex_);
        names = names_;
        shared->write([&](Capture& c) {
            c.id = next_id_++;
            c.tick_hz = s.sample_hz;
            c.names = names;
            c.source = source_->name();
            c.started_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        });
        current_ = shared;
        status_ = {};
        status_.state = s.trigger == "none" ? State::recording : State::armed;
        status_.capture_id = shared->read([](const Capture& c) { return c.id; });
    }
    const auto set_state = [&](State st) {
        std::lock_guard lock(mutex_);
        status_.state = st;
    };
    const auto finish = [&](const std::string& reason, const std::string& error = {}) {
        try {
            source_->stop();
        } catch (...) {
        }
        shared->write([](Capture& c) { c.done = true; });
        {
            std::lock_guard lock(mutex_);
            status_.state = State::idle;
            status_.words_per_s = status_.edges_per_s = 0;
            status_.stop_reason = reason;
            status_.error = error;
        }
        if (on_done_) on_done_(shared);
    };

    SourceConfig sc;
    sc.sample_hz = s.sample_hz;
    sc.glitch_ticks = static_cast<std::uint32_t>(std::llround(s.glitch_ns * 1e-9 * s.sample_hz));
    sc.threshold_v = s.threshold_v;
    try {
        source_->start(sc);
    } catch (const std::exception& e) {
        finish("error", e.what());
        return;
    }

    const double tick_hz = s.sample_hz;
    const auto duration = static_cast<std::uint64_t>(std::llround(s.duration_s * tick_hz));
    const auto pre = static_cast<std::uint64_t>(std::llround(s.pre_trigger_ms * 1e-3 * tick_hz));
    std::uint8_t mask = 0, want = 0;  // pattern trigger
    for (unsigned ch = 0; ch < kChannels && ch < s.pattern.size(); ++ch) {
        if (s.pattern[ch] == '0' || s.pattern[ch] == '1') {
            mask |= static_cast<std::uint8_t>(1u << ch);
            if (s.pattern[ch] == '1') want |= static_cast<std::uint8_t>(1u << ch);
        }
    }
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << s.trigger_channel);
    const auto fires = [&](std::uint8_t before, std::uint8_t after) {
        if (s.trigger == "rising") return !(before & bit) && (after & bit);
        if (s.trigger == "falling") return (before & bit) && !(after & bit);
        if (s.trigger == "any") return ((before ^ after) & bit) != 0;
        if (s.trigger == "pattern") return mask != 0 && (before & mask) != want && (after & mask) == want;
        return false;
    };

    WireDecoder dec;
    State state = s.trigger == "none" ? State::recording : State::armed;
    std::uint64_t end_tick = 0;  // recording: last tick (with a duration)
    std::uint8_t levels = 0;
    std::uint64_t last_tick = 0;
    std::string reason;
    std::vector<std::uint32_t> words;
    words.reserve(1 << 16);
    std::uint64_t words_n = 0, edges_n = 0;
    auto rate_t0 = clock::now();

    while (reason.empty()) {
        if (stop_) {
            reason = "stopped";
            break;
        }
        words.clear();
        try {
            source_->read(words, std::chrono::milliseconds(50));
        } catch (const std::exception& e) {
            finish("error", e.what());
            return;
        }
        words_n += words.size();
        State new_state = state;
        shared->write([&](Capture& c) {
            dec.feed(words, [&](const WireEvent& e) {
                if (!reason.empty()) return;
                if (e.kind == WireEvent::start) {
                    c.begin = c.end = 0;
                    c.initial = levels = e.levels;
                    if (state == State::recording) end_tick = duration;
                    return;
                }
                if (state == State::recording && duration > 0 && e.tick > end_tick) {
                    c.end = end_tick;
                    reason = "duration";
                    return;
                }
                const std::uint8_t before = levels;
                if (e.kind == WireEvent::gap) c.gaps.push_back({last_tick, e.tick});
                if (e.kind == WireEvent::edge || e.kind == WireEvent::gap) {
                    c.push(e.tick, e.levels);
                    ++edges_n;
                }
                levels = e.levels;
                last_tick = e.tick;
                c.end = std::max(c.end, e.tick);
                // Armed once the pre-trigger time is filled (as a scope does).
                if (state == State::armed && e.kind == WireEvent::edge && e.tick >= pre && fires(before, e.levels)) {
                    state = new_state = State::recording;
                    c.trigger = e.tick;
                    end_tick = e.tick + duration;
                    if (e.tick > pre) c.drop_before(e.tick - pre);
                }
                if (c.size() >= s.max_edges) {
                    c.truncated = true;
                    reason = "edge limit";
                }
            });
            if (state == State::armed && dec.tick() > pre) c.drop_before(dec.tick() - pre);
            if (state == State::recording && duration > 0 && dec.tick() >= end_tick && reason.empty()) {
                c.end = end_tick;
                reason = "duration";
            }
        });
        if (new_state != State::armed) set_state(new_state);

        const auto now = clock::now();
        const double dt = std::chrono::duration<double>(now - rate_t0).count();
        if (dt >= 0.5) {
            const double recorded = shared->read([](const Capture& c) { return c.seconds(c.end - c.origin()); });
            std::lock_guard lock(mutex_);
            status_.words_per_s = static_cast<double>(words_n) / dt;
            status_.edges_per_s = static_cast<double>(edges_n) / dt;
            status_.recorded_s = std::max(0.0, recorded);
            words_n = edges_n = 0;
            rate_t0 = now;
        }
    }
    {
        const double recorded = shared->read([](const Capture& c) { return c.seconds(c.end - c.origin()); });
        std::lock_guard lock(mutex_);
        status_.recorded_s = std::max(0.0, recorded);
    }
    finish(reason);
}

}  // namespace la
