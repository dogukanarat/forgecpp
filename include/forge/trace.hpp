#pragma once
// Flight recorder: always-on, low-overhead spans in a ring buffer.
//
//   void process(Block& b) {
//       FORGE_TRACE_SCOPE("process");          // records start + duration on exit
//       ...
//   }
//   forge::trace::counter("queue.depth", depth);
//
// The last N events can be downloaded at any time (/forge/api/trace,
// `forgectl trace`, console `trace`) as Chrome trace-event JSON and opened in
// https://ui.perfetto.dev to see what every thread did and how long it took.
//
// Trace context: a 128-bit trace id + 64-bit span id per thread. forgecpp puts
// it into every published message header and every control request, and
// restores it on the receiving side, so one operation can be followed across
// applications (the trace id is shown with each span in Perfetto).
//
// Names must be string literals (or strings made stable with intern()).
// Cost while recording: one atomic increment and a few stores per span.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace forge::v1 {
class TraceContext;
}

namespace forge::trace {

struct Context {
    std::array<std::uint8_t, 16> trace_id{};
    std::array<std::uint8_t, 8> span_id{};
    bool valid() const;
    std::string trace_hex() const;
    std::string span_hex() const;
};

/// Context of the calling thread (invalid when none).
Context current();
void set_current(const Context& ctx);
/// A fresh trace (random trace id and span id).
Context new_trace();
/// A child of `parent` (same trace id, new span id); new trace if parent is invalid.
Context child_of(const Context& parent);

v1::TraceContext to_proto(const Context& ctx);
Context from_proto(const v1::TraceContext& ctx);

/// RAII: makes `ctx` the thread's context for the scope.
class ContextScope {
public:
    explicit ContextScope(const Context& ctx);
    ~ContextScope();
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;

private:
    Context previous_;
};

/// Records a complete event ("ph":"X") from construction to destruction.
class Scope {
public:
    explicit Scope(const char* name, const char* category = "app");
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char* name_;
    const char* category_;
    std::int64_t start_ns_;
};

void instant(const char* name, const char* category = "app");
void counter(const char* name, double value);
/// Name the calling thread in the trace view (and for the OS, max 15 chars).
void set_thread_name(const std::string& name);
/// Stable copy of a runtime string for use as an event name.
const char* intern(std::string_view name);

/// Recording control (process-wide). Enabled by default with 65536 events.
void configure(bool enabled, std::size_t capacity);
bool enabled();

struct Dump {
    std::string chrome_json;
    std::uint64_t events = 0;
    std::uint64_t dropped = 0;  ///< overwritten by newer events
};
/// Events of the last `seconds` (0 = everything still in the buffer).
Dump dump(unsigned seconds = 0);

}  // namespace forge::trace

#define FORGE_TRACE_CONCAT2(a, b) a##b
#define FORGE_TRACE_CONCAT(a, b) FORGE_TRACE_CONCAT2(a, b)
#define FORGE_TRACE_SCOPE(...) ::forge::trace::Scope FORGE_TRACE_CONCAT(forge_trace_scope_, __LINE__)(__VA_ARGS__)
