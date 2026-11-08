# forgecpp API reference

forgecpp is a C++20 framework for Linux daemons: an application class with a
main loop, timers and a worker pool; declared configuration; controls that
appear on a web panel, forgectl, forge.v1 (ZeroMQ, HTTP), the console and
telnet; alarms, health, metrics, traces, logs, audit; streams of protobuf
messages; systemd units and the forge-hub device dashboard.

**Start here**

- [Tutorials](docs/tutorials/README.md): build a pump controller step by step.
- [Threads](docs/threading.md): where each callback runs, which call to use.
- `forge::Application`: the lifecycle and every service.
- `forge::web::Dashboard`, `forge::web::Group`: controls and commands.
- `forge::config::Schema`, `forge::Config`: configuration.
- `forge::Alarms`, `forge::Health`, `forge::metrics`, `forge::trace`: observability.
- `forge::zmq::Stream`, `forge::zmq::Client`, `forge::zmq::Receiver`: forge.v1 messaging.
- `forge::testing::AppRunner`: an application in a test (also with manual time).
- `forge::Error`: errors of requests, on every channel.

Everything in `forge::detail` is internal and may change in any release.
