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
| controls: setters, getters, buttons, commands (`setup_web`) | main loop | yes |
| console and telnet commands (`setup_console`) | main loop | yes |
| `background(work, then, on_error)`: `then`, `on_error` | main loop | yes |
| `post(fn)`, `invoke(fn)`, `call(fn)`: `fn` | main loop | yes |
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
| anywhere | a future to wait on later | `call(fn)` (runs at once when already on the loop) |
| on the loop | to do something slow (file, network, DSP) | `background(work, then)` |
| on the loop | to do it later | `after(d, fn)` / `every(d, fn)` |
| anywhere | to publish samples | `stream(...).publish(...)` (thread-safe) |
| anywhere | to raise an alarm or log | `alarms()`, `logger()` (thread-safe) |
| a real-time thread | to log without formatting or allocating there | `FORGE_RT_WARN(logger(), "late by {} us", us)` (`<forge/log_rt.hpp>`; drops when full, never blocks) |

`invoke()` runs `fn` inline when called on the loop, so it is safe everywhere.

## What the framework checks for you

- **The controls belong to the loop.** After `setup_web()`, a `web::Dashboard`
  value method (`get`, `set`, `state`, `press`, `call`, ...) called from
  another thread throws `std::logic_error` naming the method, instead of
  racing.
- **No self-deadlock.** `call(fn).get()` on the loop runs `fn` at once.
- **Slow tasks are named.** A loop task longer than `[forge].slow_task_ms`
  (200) logs a warning (at most every 10 s) and counts in
  `forge.loop.slow_tasks`; `forge.loop.lag_seconds` shows how long tasks wait.
  The trace (`/forge/` → System → trace) shows which one.
- **A stuck loop does not take the control channels with it.** Commands on a
  link (a NACK with EAGAIN) and the framework's HTTP routes (503) give up after `[forge].request_timeout_ms`,
  and `/health` reports `"stalled"`; the systemd watchdog restarts a loop
  that stays stuck.

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
