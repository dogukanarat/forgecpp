# Threads in a forgecpp application

One rule covers almost everything:

> **The application's state belongs to its main loop.** Code that runs
> elsewhere reaches it through `invoke()` (wait for a result) or `post()`
> (fire and forget). Heavy work leaves the loop with `background()`.

The main loop is a single thread that runs one task at a time. Nothing else
touches your members there, so you need no locks for them.

## Where does my code run?

| Code | Thread | May touch the application's state directly? |
|---|---|---|
| `on_start()`, `on_stop()`, `on_reload()`, hooks | main loop | yes |
| `every()`, `after()` callbacks | main loop | yes |
| controls: setters, getters, buttons, actions (`setup_controls`) | main loop | yes |
| console and telnet commands (`setup_console`) | main loop | yes |
| `background(work, then, on_error)`: `then`, `on_error` | main loop | yes |
| `post(fn)`, `invoke(fn)`: `fn` | main loop | yes |
| `background(work, then)`: `work` | worker pool | **no**: compute, return the result |
| `pool().detach_task(...)` | worker pool | **no** |
| your own HTTP routes (`setup_http`) | HTTP threads | **no**: `invoke_for(request_timeout(), ...)` |
| `zmq::Receiver` callbacks | the receiver's thread | **no**: `post()` |
| your own `std::thread` (e.g. an RX loop) | that thread | **no**: `post()`; `stream().publish()` is fine |
| `EventBus` handlers (`events().subscribe`) | whoever published | as the publisher |

## Which call do I use?

| You are… | and want… | use |
|---|---|---|
| anywhere but the loop | a value from the state | `invoke([&] { return x_; })` |
| an HTTP handler or a command on a link | the same, without hanging if the loop is stuck | `invoke_for(request_timeout(), ...)` (503 "main loop busy") |
| anywhere but the loop | to change the state, not waiting | `post([=] { x_ = v; })` |
| on the loop | to do something slow (file, network, DSP) | `background(work, then)` |
| on the loop | to do it later | `after(d, fn)` / `every(d, fn)` |
| on the loop | steps with waits between them (power up, retry, give up) | a state machine ([below](#sequences-a-state-machine)) |
| anywhere | to publish samples | `stream(...).publish(...)` (thread-safe) |
| anywhere | to raise an alarm or log | `alarms()`, `logger()` (thread-safe) |
| a real-time thread | to log without formatting or allocating there | `FORGE_RT_WARN(logger(), "late by {} us", us)` (`<forge/log_rt.hpp>`; drops when full, never blocks) |

`invoke()` runs `fn` inline when called on the loop, so it is safe everywhere.

## Sequences: a state machine

Work in steps with waits between them (switch a payload on, give it two
seconds to boot, check that it answers, try three times, then give up) is
written as an explicit state machine: an `enum` of the states, and one
`enter()` that every transition goes through.

- **The state can be seen.** It is a member: the panel, `forgectl get` and
  the telemetry show it ("checking, attempt 2"), and the log has every
  transition.
- **It can be stopped.** An operator or a command calls `abort()`; the
  pending timer is cancelled in `enter()`, so nothing of the old sequence
  runs later.
- **It is tested with the manual clock.** `r.advance(2s)` moves it one step
  at a time, without sleeping.

The rules that keep it simple:

1. **Every wait is a state.** "Off for a second before the next attempt" is
   `retrying`, not a timer hidden in another state.
2. **`enter()` cancels the pending timer** before it starts the next one.
3. **Answers from the pool are checked against the attempt** they belong
   to: a late answer of an aborted attempt is ignored.
4. **The state is a control**, so it is visible without a debugger.

```cpp
class Payload : public forge::Component {
public:
    enum class State { off, booting, checking, retrying, ready, failed };

    Payload() : Component("payload") {}

    void power_up() {  // a button, a command: on the main loop
        attempts_ = 0;
        enter(State::booting);
    }
    void abort() { enter(State::off); }
    State state() const { return state_; }

protected:
    void setup_controls(forge::Controls& ui) override {
        auto& g = ui.group("Payload");
        g.value("state", "State", [this] { return to_string(state_); });  // the panel and telemetry see it
        g.value("attempts", "Attempts", attempts_);
        g.button("power_up", "Power up", [this] { power_up(); });
        g.button("abort", "Abort", [this] { abort(); });
    }

private:
    // Every transition goes through here: one place to log, to cancel the
    // pending timer and to start what the new state needs.
    void enter(State next) {
        app().cancel(timer_);
        logger()->info("payload: {} -> {}", to_string(state_), to_string(next));
        state_ = next;
        switch (state_) {
            case State::off: switch_power(false); break;
            case State::booting:
                ++attempts_;
                switch_power(true);
                timer_ = app().after(2s, [this] { enter(State::checking); });
                break;
            case State::checking:
                // The probe blocks (a bus transaction): off the loop, the answer back on it.
                app().background([] { return probe(); }, [this, attempt = attempts_](bool alive) {
                    // An answer for an attempt that is over (aborted, started again): ignore it.
                    if (state_ != State::checking || attempts_ != attempt) return;
                    if (alive) {
                        enter(State::ready);
                    } else {
                        enter(attempts_ < 3 ? State::retrying : State::failed);
                    }
                });
                break;
            case State::retrying:  // off for a second, then the next attempt
                switch_power(false);
                timer_ = app().after(1s, [this] { enter(State::booting); });
                break;
            case State::ready: set_health(forge::HealthState::ok, "ready"); break;
            case State::failed:
                switch_power(false);
                set_health(forge::HealthState::failed, "no answer after 3 attempts");
                app().alarms().raise("payload.power", forge::Severity::error, "payload does not answer");
                break;
        }
    }

    static const char* to_string(State s) {
        switch (s) {
            case State::off: return "off";
            case State::booting: return "booting";
            case State::checking: return "checking";
            case State::retrying: return "retrying";
            case State::ready: return "ready";
            case State::failed: return "failed";
        }
        return "?";
    }

    void switch_power(bool on) { powered_ = on; }  // a GPIO in a real payload
    static bool probe() { return probe_answers.load(); }

    State state_ = State::off;
    int attempts_ = 0;
    bool powered_ = false;
    forge::Scheduler::Id timer_ = 0;

public:
    static inline std::atomic<bool> probe_answers{false};  // what probe() answers in the test
};
```

The component and its test: `tests/test_state_machine.cpp`.

Why not `co_await`? C++20 coroutines would write the same sequence as one
function, but they keep the state inside the coroutine frame, where neither
the panel nor a command can see or stop it, and the compiler does not catch
a reference that dies while the coroutine waits. They are deferred
([ROADMAP.md](ROADMAP.md)) until an application has sequences a state
machine handles badly.

## What the framework checks for you

- **The controls belong to the loop.** After `setup_controls()`, a `web::Dashboard`
  value method (`get`, `set`, `state`, `press`, `call`, ...) called from
  another thread throws `std::logic_error` naming the method, instead of
  racing.
- **No self-deadlock.** `invoke(fn)` on the loop runs `fn` at once.
- **Slow tasks are named.** A loop task longer than `[loop].slow_task_ms`
  (200) logs a warning (at most every 10 s) and counts in
  `forge.loop.slow_tasks`; `forge.loop.lag_seconds` shows how long tasks wait.
  The trace (`/forge/` → System → trace) shows which one.
- **A stuck loop does not take the control channels with it.** Commands on a
  link (a NACK with EAGAIN) and the framework's HTTP routes (503) give up after `[loop].request_timeout_ms`,
  and `/health` reports `"stalled"`; the systemd watchdog restarts a loop
  that stays stuck.

## Limits and bounds

- **One `Application` per process.** The metrics registry, the log state, the
  trace recorder, the stream source name, the crash handler and the console
  are process-wide; `run()` blocks the handled signals for the whole process
  (and puts the mask back when it returns). Two applications in one process
  share them.
- **The main loop's queue is bounded**: `[loop].max_pending_tasks` (10000).
  Beyond it `post()` returns false and counts `forge.loop.dropped`;
  `invoke()` and the request handlers answer 503 "main loop busy".
  `forge.loop.queued` shows the depth.
- **Streams are bounded per stream and in bytes**: `send_hwm` messages per
  stream (1000) and `SocketOptions::send_budget_bytes` (64 MiB) for all the
  streams of one publisher; beyond either, `publish()` drops and counts.
- **Stacks**: a command handler runs with its request and reply on the stack
  (`forge_controls_reply` is about 12 kB), and a thread that dispatches
  commands keeps a 64 kB buffer; a thread of your own that calls
  `serial::Link::call()` or `tmtc::Dispatcher::handle()` needs at least 256
  kB of stack. Threads of the framework and `std::thread` have 8 MB.
- **Idle threads sleep.** The I/O threads (publisher, receiver, command
  server, signals) wait on their sockets and an eventfd and wake only for
  work; the main loop wakes only for a task or a timer.

## Time

Read time with `now()`, not `std::chrono::steady_clock::now()`: in tests
(`AppRunner({}, {.manual_time = true})`, `r.advance(10s)`) the clock only
moves when the test says so, and every timer runs at the time it was due.

## A typical shape

```cpp
class Pump : public forge::Application {
    double pressure_ = 0;                       // main loop only
    std::thread reader_;

    void on_start() override {
        reader_ = std::thread([this] {          // its own thread
            while (!stopping()) {
                const double p = sensor_.read();          // blocking I/O
                post([this, p] { on_pressure(p); });      // into the loop
            }
        });
        every(1s, [this] { check_limits(); });            // on the loop
    }
    void on_pressure(double p) { pressure_ = p; }        // on the loop
    void export_log() {                                   // a command: on the loop
        background([log = history_] { return write_csv(log); },     // pool
                   [this](std::string file) { notify_web("saved " + file); },   // loop
                   [this](const std::string& why) { notify_web("not saved: " + why); });  // loop, if it threw
    }
};
```
