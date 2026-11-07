#include "forge/trace.hpp"

#include "forge/v1/common.pb.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <random>
#include <set>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace forge::trace {
namespace {

using Clock = std::chrono::steady_clock;

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

std::uint32_t tid() {
    thread_local const std::uint32_t id = static_cast<std::uint32_t>(::syscall(SYS_gettid));
    return id;
}

enum class Type : std::uint8_t { complete, instant, counter };

struct Event {
    std::atomic<std::uint64_t> seq{0};  ///< 2*index+1 while written, 2*index+2 when complete
    std::int64_t start_ns = 0;
    std::int64_t dur_ns = 0;
    const char* name = nullptr;
    const char* category = nullptr;
    double value = 0;
    std::uint64_t trace_lo = 0;  ///< low 64 bits of the trace id (shown as args.trace)
    std::uint32_t tid = 0;
    Type type = Type::complete;
};

struct Recorder {
    std::atomic<bool> enabled{true};
    std::atomic<std::uint64_t> head{0};
    std::size_t capacity = 0;
    std::unique_ptr<Event[]> events;
    std::mutex names_mutex;
    std::unordered_map<std::uint32_t, std::string> thread_names;
    std::set<std::string> interned;

    Recorder() { resize(65536); }

    void resize(std::size_t n) {
        capacity = std::max<std::size_t>(n, 1024);
        events = std::make_unique<Event[]>(capacity);
        head = 0;
    }

    void record(Type type, const char* name, const char* category, std::int64_t start, std::int64_t dur,
                double value) {
        if (!enabled.load(std::memory_order_relaxed)) return;
        const std::uint64_t idx = head.fetch_add(1, std::memory_order_relaxed);
        Event& e = events[idx % capacity];
        e.seq.store(2 * idx + 1, std::memory_order_release);
        e.start_ns = start;
        e.dur_ns = dur;
        e.name = name;
        e.category = category;
        e.value = value;
        e.tid = tid();
        e.type = type;
        const auto ctx = current();
        std::uint64_t lo = 0;  // bytes 8..15 in order: same digits as trace_hex()
        for (std::size_t i = 8; i < 16; ++i) lo = (lo << 8) | ctx.trace_id[i];
        e.trace_lo = lo;
        e.seq.store(2 * idx + 2, std::memory_order_release);
    }
};

Recorder& recorder() {
    static Recorder r;
    return r;
}

thread_local Context tls_context;

std::mt19937_64& rng() {
    thread_local std::mt19937_64 g{std::random_device{}() ^ (static_cast<std::uint64_t>(tid()) << 32)};
    return g;
}

template <std::size_t N>
std::string hex(const std::array<std::uint8_t, N>& a) {
    static const char* digits = "0123456789abcdef";
    std::string s(2 * N, '0');
    for (std::size_t i = 0; i < N; ++i) {
        s[2 * i] = digits[a[i] >> 4];
        s[2 * i + 1] = digits[a[i] & 15];
    }
    return s;
}

void fill_random(std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; i += 8) {
        const std::uint64_t r = rng()();
        std::memcpy(p + i, &r, std::min<std::size_t>(8, n - i));
    }
}

std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += fmt::format("\\u{:04x}", static_cast<int>(c));
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace

bool Context::valid() const {
    return std::any_of(trace_id.begin(), trace_id.end(), [](std::uint8_t b) { return b != 0; });
}
std::string Context::trace_hex() const { return hex(trace_id); }
std::string Context::span_hex() const { return hex(span_id); }

Context current() { return tls_context; }
void set_current(const Context& ctx) { tls_context = ctx; }

Context new_trace() {
    Context c;
    fill_random(c.trace_id.data(), c.trace_id.size());
    fill_random(c.span_id.data(), c.span_id.size());
    return c;
}

Context child_of(const Context& parent) {
    if (!parent.valid()) return new_trace();
    Context c = parent;
    fill_random(c.span_id.data(), c.span_id.size());
    return c;
}

v1::TraceContext to_proto(const Context& ctx) {
    v1::TraceContext t;
    if (!ctx.valid()) return t;
    t.set_trace_id(std::string(reinterpret_cast<const char*>(ctx.trace_id.data()), ctx.trace_id.size()));
    t.set_span_id(std::string(reinterpret_cast<const char*>(ctx.span_id.data()), ctx.span_id.size()));
    return t;
}

Context from_proto(const v1::TraceContext& t) {
    Context c;
    if (t.trace_id().size() == c.trace_id.size()) std::memcpy(c.trace_id.data(), t.trace_id().data(), c.trace_id.size());
    if (t.span_id().size() == c.span_id.size()) std::memcpy(c.span_id.data(), t.span_id().data(), c.span_id.size());
    return c;
}

ContextScope::ContextScope(const Context& ctx) : previous_(tls_context) { tls_context = ctx; }
ContextScope::~ContextScope() { tls_context = previous_; }

Scope::Scope(const char* name, const char* category) : name_(name), category_(category), start_ns_(now_ns()) {}
Scope::~Scope() {
    const auto end = now_ns();
    recorder().record(Type::complete, name_, category_, start_ns_, end - start_ns_, 0);
}

void instant(const char* name, const char* category) {
    recorder().record(Type::instant, name, category, now_ns(), 0, 0);
}

void counter(const char* name, double value) { recorder().record(Type::counter, name, "counter", now_ns(), 0, value); }

void set_thread_name(const std::string& name) {
    {
        auto& r = recorder();
        std::lock_guard lock(r.names_mutex);
        r.thread_names[tid()] = name;
    }
    ::pthread_setname_np(::pthread_self(), name.substr(0, 15).c_str());
}

const char* intern(std::string_view name) {
    auto& r = recorder();
    std::lock_guard lock(r.names_mutex);
    return r.interned.emplace(name).first->c_str();
}

void configure(bool on, std::size_t capacity) {
    auto& r = recorder();
    r.enabled = false;
    if (capacity != r.capacity) {
        // Writers may still be inside record(): keep the old buffer alive.
        static std::vector<std::unique_ptr<Event[]>> retired;
        std::lock_guard lock(r.names_mutex);
        retired.push_back(std::move(r.events));
        r.resize(capacity);
    }
    r.enabled = on;
}

bool enabled() { return recorder().enabled.load(); }

Dump dump(unsigned seconds) {
    auto& r = recorder();
    const std::uint64_t head = r.head.load(std::memory_order_acquire);
    const std::uint64_t first = head > r.capacity ? head - r.capacity : 0;
    const std::int64_t since = seconds > 0 ? now_ns() - static_cast<std::int64_t>(seconds) * 1000000000LL : 0;
    // steady clock -> trace microseconds; Perfetto only needs a common origin.
    const std::int64_t origin = now_ns();
    const auto pid = ::getpid();

    Dump d;
    d.dropped = first;
    std::string out = "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[";
    bool comma = false;
    auto add = [&](const std::string& s) {
        if (comma) out += ',';
        out += s;
        comma = true;
    };
    {
        std::lock_guard lock(r.names_mutex);
        for (const auto& [t, n] : r.thread_names) {
            add(fmt::format(R"({{"ph":"M","name":"thread_name","pid":{},"tid":{},"args":{{"name":"{}"}}}})", pid, t,
                            escape(n)));
        }
    }
    for (std::uint64_t i = first; i < head; ++i) {
        const Event& e = r.events[i % r.capacity];
        const std::uint64_t s1 = e.seq.load(std::memory_order_acquire);
        if (s1 != 2 * i + 2) continue;  // being written or already overwritten
        Event copy;
        copy.start_ns = e.start_ns;
        copy.dur_ns = e.dur_ns;
        copy.name = e.name;
        copy.category = e.category;
        copy.value = e.value;
        copy.trace_lo = e.trace_lo;
        copy.tid = e.tid;
        copy.type = e.type;
        if (e.seq.load(std::memory_order_acquire) != s1) continue;
        if (copy.start_ns < since || !copy.name) continue;
        const double ts = static_cast<double>(copy.start_ns - origin) / 1000.0;
        const std::string trace = copy.trace_lo ? fmt::format(R"(,"args":{{"trace":"{:016x}"}})", copy.trace_lo) : "";
        switch (copy.type) {
            case Type::complete:
                add(fmt::format(R"({{"ph":"X","name":"{}","cat":"{}","pid":{},"tid":{},"ts":{:.3f},"dur":{:.3f}{}}})",
                                escape(copy.name), escape(copy.category ? copy.category : ""), pid, copy.tid, ts,
                                static_cast<double>(copy.dur_ns) / 1000.0, trace));
                break;
            case Type::instant:
                add(fmt::format(R"({{"ph":"i","s":"t","name":"{}","cat":"{}","pid":{},"tid":{},"ts":{:.3f}{}}})",
                                escape(copy.name), escape(copy.category ? copy.category : ""), pid, copy.tid, ts,
                                trace));
                break;
            case Type::counter:
                add(fmt::format(R"({{"ph":"C","name":"{}","pid":{},"tid":{},"ts":{:.3f},"args":{{"value":{}}}}})",
                                escape(copy.name), pid, copy.tid, ts, copy.value));
                break;
        }
        ++d.events;
    }
    out += "]}";
    d.chrome_json = std::move(out);
    return d;
}

}  // namespace forge::trace
