# Field notes

What comes up in the first weeks with a forgecpp daemon on a device, and what
to do about it. Each note is short; the pages it points to have the details.

## Ports on a device

Every forgecpp program binds the endpoints its configuration names. Two
programs with the same configuration collide on every one of them, so a device
has a port plan, kept in one place (the image's configuration files, or this
table in the project's README):

| Program | HTTP | `[forge].control` | `[forge].events` | `[forge].data` | telnet |
|---|---|---|---|---|---|
| forge-hub | 80 | 5551 | 5552 | 5553 | off |
| radio-controller | 8080 | 5557 | 5559 | 5555 (+5556 TX) | 2323 |
| spectrum-analyzer | 8081 | 5567 | 5569 | 5566 | 2324 |
| qam-modem | 8082 | 5577 | 5579 | 5575 | 2325 |
| channel-emulator | 8083 | 5587 | 5589 | 5585 | 2326 |

The rule the examples follow: one block of ten per program (`55x0`..`55x9`:
control `x7`, events `x9`, data `x5`), HTTP from 8080 up, telnet from 2323 up.
forge-hub shows every program's endpoints, so an operator never has to
remember them; for programs on the same device, `ipc://` endpoints need no
plan at all:

```toml
[forge]
control = "ipc:///run/radio-controller/control"   # RuntimeDirectory= of the unit
```

A `TEMPLATE` unit (`radio-controller@rx2`) gets one configuration file per
instance (`rx2.toml`), and so its own ports.

Addresses are static: a program finds another one through its configuration
(`[emulator].input = "tcp://127.0.0.1:5555"`), not through discovery
([protocol.md](protocol.md#what-is-guaranteed)).

## Debugging on the device

The generated unit is hardened (`ProtectSystem=full`, `PrivateTmp`,
`NoNewPrivileges`, ...; [commands.md](commands.md#the-unit)). For a debugger:

- `gdbserver :2345 --attach $(pidof radio-controller)` from a shell on the
  device (the unit does not forbid ptrace from outside it); from the SDK:
  `gdb build/radio-controller -ex 'target remote device:2345'`.
- Core dumps: `[crash]` writes a report (`<app>-crash.txt`: signal, address,
  backtrace, the last log lines) and lets the process die as before, so
  `systemd-coredump` (`coredumpctl gdb radio-controller`) still works. The
  report is in `$STATE_DIRECTORY` (`/var/lib/<app>`), and the next start
  raises the alarm `forge.crashed` with its path.
- The flight recorder: `forgectl -e http://device:8080 trace -o trace.json`
  and ui.perfetto.dev show the last seconds of every thread; a stuck main
  loop shows as one long `loop.task` span (`[loop].slow_task_ms` logs its
  name).
- Logs: `journalctl -u radio-controller -f`; `forgectl logs -n 200`; the
  panel's log card filters by level and text.

## Persisted controls and renames

Controls marked `.persist()` are saved in `[state].file` (`state.json`) by
their id. Renaming a control loses its saved value: the old id is ignored
with a warning at start ("state: cannot restore 'old'"). To migrate, keep the
old id, or read the old value once in `on_start()` from the file and set the
new control.

## Time without an RTC

Many boards start at 1970 until NTP or GNSS sets the clock. forgecpp uses the
steady clock for everything that must not jump (timers, `uptime()`, loop lag,
rate limits) and the wall clock only for display and for the time stamps in
stream headers, alarms, audit and log records. A receiver orders messages by
their sequence number, not by their time. Timer logic in applications should
use `now()` (the application's clock) for the same reason, and so that tests
can drive it ([testing](tutorials/05-testing.md)).

## Metric names

Metrics are named with dots (`forge.loop.lag_seconds`, `radio.rx.overflows`);
the panel and `forgectl metrics` show them so. Prometheus does not allow dots,
so `/metrics` exports them with underscores (`forge_loop_lag_seconds`,
`radio_rx_overflows`); labels are kept (`forge_stream_sent{topic="iq"}`). In
Grafana, use the underscore form.

## When the main loop is busy

A control change, a command or `/health` answers 503 "main loop busy" after
`[loop].request_timeout_ms` (2000). The cause is a task on the main loop that
runs too long: `[loop].slow_task_ms` logs its name, the trace shows it. Move
the work off the loop with `background()` or a `worker()`
([threading.md](threading.md)). The queue of the loop is bounded
(`[loop].max_pending_tasks`); beyond it `forge.loop.dropped` counts.

## A serial line that comes and goes

`[serial].device` is opened again every second when it is missing or went
away (a USB adapter unplugged); health shows `serial` as failed in between,
and the panel's health card and `/health` say so. Nothing else is needed.

## Which build am I running?

`forgectl info` (or `GET /forge/api/info`) gives the program's version, the
forgecpp version it was built with, its endpoints and its uptime; the
`--version` flag prints the same from the binary.
