# 4. Threads and heavy work

The application's state belongs to its main loop: `on_start`, timers,
controls, commands and console commands all run there, one at a time, so
they need no locks. The full picture: [docs/threading.md](../threading.md).

**Slow work leaves the loop.** While a task runs, nothing else does: no
control answers, no timer fires. Writing a file is slow:

```cpp
void export_history() {
    background([h = history] { return write_csv(h); },     // on a worker: a copy, no members
               [this](std::string file) {                  // back on the main loop
                   notify_web("History saved to " + file, "success");
               });
}
```

A task over `[loop].slow_task_ms` (200 ms) is logged with this advice.

**Other threads** (a sensor reader, a ZeroMQ receiver, your HTTP routes)
reach the state through the loop:

```cpp
reader_ = std::thread([this] {
    while (!stopping()) {
        const double p = sensor_.read();          // blocking I/O on its own thread
        post([this, p] { pressure_bar = p; });    // the state changes on the loop
    }
});
// in an HTTP handler: wait for a value, but not forever
server.get_json("/api/pressure", [this](const auto&) {
    return invoke_for(request_timeout(), [this] { return forge::json{{"bar", pressure_bar}}; });
});
```

Mistakes the framework catches: using the panel's controls from another
thread throws, `invoke()` on the loop does not deadlock, a stuck loop
makes requests answer 503 instead of hanging. Next: [testing](05-testing.md).
