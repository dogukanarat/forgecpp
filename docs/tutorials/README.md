# Tutorials

Each step builds on the previous one. Together they build
[`examples/pump`](../../examples/pump): a pump controller with a web panel,
remote control, an alarm, a data stream, tests and a systemd unit. That code
is compiled and tested with forgecpp, so what you read here works.

1. [Your first application](01-first-application.md): build, run, the panel, forgectl
2. [Controls and commands](02-controls.md): one declaration for the panel, forgectl, the console
3. [Configuration, timers and alarms](03-config-timers-alarms.md)
4. [Threads and heavy work](04-threads.md)
5. [Testing](05-testing.md): a client's view, and time you control
6. [Streams and Python clients](06-streams.md)
7. [Deploying: systemd, forge-hub, Yocto](07-deploy.md)

Reference: [the README's framework tour](../../README.md#framework-tour),
[threads](../threading.md), and the API reference (`cmake --build build
--target forgecpp_docs`, then `build/docs/html/index.html`).
