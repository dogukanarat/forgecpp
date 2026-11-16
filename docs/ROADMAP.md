# Roadmap

Where forgecpp stands, what comes next, and what was done. The order is by
risk to a running system: hardware first, then security, then a stable API.

## Where it stands (0.4)

- The framework, forge-hub, the tooling and the examples are complete and
  tested on PCs, against simulators, and cross-built for the Zynq-7000.
- The hardware paths (GPIO, CAN, the AD936x radio, the logic analyzer's FPGA
  core) are written to the kernel and register interfaces but not yet run on
  the boards.
- forgecpp is used in the lab. Security work is planned before production use
  (below); until then, run it on trusted networks only.
- The C++ API may still change in a minor version (0.5); messages and
  configuration keys change only compatibly ([API.md](API.md)).

## Next

### 1. Release 0.4.0

`tools/release.sh 0.4.0` ([development.md](development.md#versions)): the
CHANGELOG dated, the API records written, the tag `v0.4.0`. CI green on the
public repository first (the workflows have not run while the repository was
private).

### 2. Hardware bring-up

**GPIO and CAN on real hardware.** The kernel paths (GPIO ioctls, `CAN_RAW`
and `CAN_ISOTP` sockets) build and are written to the kernel headers; the
tests run on the simulated chip and on socket pairs, because the build machine
has neither `/dev/gpiochip*` nor `AF_CAN`. On the board: `payload-power` with
`[payload].chip = "gpiochip0"`, `can-node` with `sensor.py` on vcan and on a
real bus. zephyrus has no CAN transport yet (Zephyr's ISO-TP plus its COBS
framing).

**The logic analyzer on an Arty Z7** ([its FPGA design](../examples/logic-analyzer/fpga/README.md)).
The input stage and the change encoder are written and simulated. To do: the
AXI-Lite register block, the DDR writer (or AXI DataMover), the top level and
the Vivado block design; the device tree node and reserved memory; the shield.
Then measure how many words per second the Cortex-A9 reads and decodes.

**The radio examples on a ZC702 / Z7020 + FMCOMMS5.**

Chosen: **AMD EDF 2026.1 (Yocto scarthgap) + a thin `yocto/meta-forgecpp-bsp`
layer**, not the whole meta-adi.

Why 2026.1 and not 2026.2 (wrynose): ADI HDL `main` needs Vivado 2026.1, which
is what EDF 2026.1's SDT tools (`gen-machine-conf` `xlnx_rel_v2026.1`) match;
ADI's 6.12 kernel is built with scarthgap's GCC 13.5 as ADI tests it; meta-adi
`main` is scarthgap only (no wrynose yet). EDF 2026.1 has the Zynq-7000
Cortex-A9 machine and `zynq-zc702-sdt-full`. scarthgap is supported until
April 2028.

Why not all of meta-adi: it is written for PetaLinux/XSCT (depends on
meta-xilinx-tools, which EDF does not use; its device-tree merging does not
fit the SDT flow). What we need from it is small and MIT licensed: the ADI
kernel source switch and `libad9361-iio` (multi-chip sync of the FMCOMMS5's
two AD9361). libiio comes from meta-oe (0.25), or from this layer's backfill (0.26) without it.

The layer:

1. Kernel: ADI linux 6.12 at a fixed commit (not AUTOREV), AMD's 6.12
   `linux-xlnx` recipe selected (EDF 2026.1 defaults to 6.18),
   `zynq_xcomm_adv7511_defconfig` plus our fragment.
2. Device tree for Linux from the ADI kernel:
   `zynq-zc702-adv7511-ad9361-fmcomms5.dts` (matches the HDL design); the SDT
   is only used for FSBL / boot.bin.
3. Machine `zc702-fmcomms5`: ADI HDL `projects/fmcomms5/zc702` built with
   Vivado 2026.1 → XSA → SDT → `gen-machineconf`; boot.bin from EDF's
   `xilinx-bootbin`.
4. `libad9361-iio`; image: `edf-linux-disk-image` + forgecpp, forge-hub and the
   radio applications.
5. `yocto/kas/zc702-fmcomms5.yml` with every layer pinned to a commit; nightly CI.

Work: about 2–3 days for the layer, half a day to a day for the HDL/XSA
(Vivado, on the user's side), half a day for machine + boot.bin, 2–3 days on
the board. Fallbacks: for the first boot, ADI Kuiper's prebuilt `BOOT.BIN`
with our kernel and rootfs; EDF features that expect the 6.18 kernel (FPGA
overlays) need checking. Moving to wrynose later means porting only this
layer and regenerating the machine (about 2–4 days), once ADI supports it or
it is needed.

First checks on the board: clean-loop EVM (expect −30 to −35 dB), DC / LO
leakage at the centre of the constellation (fix: a digital TX offset), CPU
(skipped RX buffers; `equalizer = false`), RTT and late TX blocks.

### 3. Security

Before production use:

- Secure defaults: without `[auth]`, the control endpoint, telnet and the
  `[forge]` endpoints only on localhost.
- TLS for HTTP (or a terminating proxy), telnet replaced or tunnelled through ssh.
- Hub registration over a Unix socket with `SO_PEERCRED`.
- A review of the roles of every command, and a role for commands on a link
  (c2 frames carry no credentials; today they get `[auth].default_role`).

### 4. Toward 1.0

| Item | Notes |
|---|---|
| Hidden symbol visibility with an export macro, `abidiff` in CI | The header-only libraries in the public headers keep state in templates (quill's frontend, moodycamel's thread registry); hidden visibility would give the application and the library each their own copy. First the facades (pool, channel), then visibility ([API.md](API.md)). |
| A full bitbake build and `yocto-check-layer` | The recipes are written against the scarthgap classes, parsed and their dependency graph resolved on every change to them (`yocto-check`); the weekly kas job is where they get built. |
| A CMake superbuild, or vcpkg / Conan packages | `install.sh` builds the dependencies from submodules and works; package managers would make it easier to adopt. |
| protobuf 21.12 → current | Needs abseil on the build machine; only `protoc` is used. |

## Later

- Asio-based I/O; a WebSocket server (the panel uses Server-Sent Events today).
- Connector modules: MQTT, Modbus, OPC UA.
- Python bindings of the framework (the `forge` Python package covers clients today).
- `forge::Remote` finding a program through forge-hub (needs a hub command that
  lists registrations with their endpoints; the port plan in
  [field-notes.md](field-notes.md) covers today's need).
- `async`/`await` (C++20 coroutines): planned and dropped for now. One example
  (qam-modem's radio set-up) would read better, nothing else. Sequences are
  explicit state machines ([threading.md](threading.md#sequences-a-state-machine)):
  the state is visible on the panel and in telemetry and can be aborted, which a
  coroutine frame hides; and C++ does not catch a reference that dies while a
  coroutine waits. Revisit when an application has sequences a state machine
  handles badly; the design is in the CHANGELOG (0.4.0, "Added (docs)").

Not planned: more than two `commands` entries per reply page (each entry is
about 6 kB; a larger page would not fit a zephyrus peer's default buffer).

## Done

### 0.4.0

- **C++20** for the library, its exported targets and the examples.
- **linenoise** instead of replxx (unmaintained since 2021); **Quill** instead
  of spdlog, behind a forgecpp log facade; `std::format` instead of fmt.
- **Messages stay .proto**, the runtime is **nanopb with static structs** (every
  string, bytes and repeated field bounded: no allocation, the same file builds
  for zephyrus). forge.v1 is replaced by **c2 + tmtc**: numbered request/reply
  pairs, c2 frames on links, JSON on the web, one handler for both.
- The **serial link** (c2 on a UART with zephyrus's COBS framing, checked against
  zephyrus's C code), **CAN** (raw frames, c2 over ISO-TP) and **GPIO** on the
  kernel interfaces; **SQLite** with a store-and-forward backlog, **LZ4** and
  **sqlite_orm** as optional components.
- `forge::Worker` and `Application::worker()`, `background()` with an error path,
  atomics and metrics bound to the panel, Dependabot, clang-tidy, fuzzing,
  hardened systemd units, `tools/check.sh`, presets, `tools/release.sh`, benchmarks.
- Examples: three-threads, payload-power, can-node, telemetry-store, fault-log,
  logic-analyzer.

A review of the whole repository (C++ core, developer experience, build and
Yocto, tests, protocol and runtime; 68 findings) was worked through before
the release. In short: the timer cancel race, the HTTP alarm acknowledgement
that skipped roles and the audit trail, the serial line that never came back,
fsync on the main loop, `Status` messages that could dangle, two
`Subscription` types, metric callbacks under the registry lock, a stop handler
that skipped the others; bounded queues (bytes, per stream, the main loop's)
and no idle polling; protocol guarantees written down and the nanopb bound
rule corrected; labelled metrics for the links and streams; `setup_controls`,
`Group::action`, `/forge/api/commands`, `[serial]` and `[loop]`; the smallest
scaffold, a `Component` in an example, pip for the Python clients, field
notes; the Yocto layer on the distribution's libraries with ptest and
nativesdk tools; toml++, the pool and the channel out of the application
header. The CHANGELOG lists every item.

### The design review before 0.4

| Finding | Fix |
|---|---|
| Two restart authorities (forge-hub and systemd) | Restarts are systemd's only. `forge_add_app(... RESTART MAX_RESTARTS RESTART_WINDOW RESTART_DELAY WATCHDOG)` generates the unit; forge-hub reads state, `NRestarts`, `Result` and the policy with `systemctl show` and starts / stops / restarts units on request. The hub no longer starts processes or reads their environment. Hangs: systemd's watchdog (the main loop feeds it). |
| Slow, timing-dependent hub tests (60 s) | systemd behind an interface with a fake in tests, 20 ms ticks, sleep processes instead of applications: 2 s. Whole suite: 66 s -> 5 s. |
| Sanitizers only run by hand | CI job `sanitizers`: ASan+UBSan and TSan, the bundled libraries instrumented too, all tests. ccache for every CI job. |
| `invoke()` shared exception objects between threads (TSan) | Errors are described on the main loop and thrown anew on the caller's thread. |
| A stuck main loop hung forgectl and the panel | `invoke_for(timeout, fn)`: forge.v1 and the framework's HTTP routes wait at most `[loop].request_timeout_ms` (2000) for the main loop, then answer `503 main loop busy` with the loop's lag (metric `forge.control.timeouts`); a request that timed out is never applied later. info, health, metrics, logs, audit, trace, alarms and streams do not need the loop. |
| `[forge].control` served one request at a time (REP) | `zmq::Server` is a ROUTER socket with an I/O thread and a worker pool (`[forge].control_workers`, 2); replies go back to the client that asked, requests that waited too long for a worker (or beyond 256 waiting) get 503 without being handled. Clients and messages unchanged. |
| The control service threw an HTTP type (`http::HttpError`) on every channel | `forge::Error{status, message, details}` in `forge/error.hpp`, thrown by `ControlService`, `web::Dashboard`, forge-hub and `invoke_for`; HTTP, forge.v1 and the console map it. `http::HttpError` derives from it (throwing it still works; `catch (const http::HttpError&)` around control calls becomes `catch (const forge::Error&)`). |
| Defaults written twice (schema and every `get_or`) | `Schema::apply_defaults()` fills in every declared key after loading and reloading; declared keys are read with `cfg.value<T>(key)` (framework, forge-hub, all examples migrated; the small examples declare their keys now). Environment-dependent defaults are `.computed()`. A test walks `src/`, `hub/src/`, `examples/` and fails on `get_or` of a declared key. |
| `application.cpp` was 1600 lines doing everything | Internal services in `src/app/` own their state: `HttpFrontend` (server, framework routes, web events and logs), `LayoutStore`, `Messaging` (events, streams, control endpoint), `RemoteConsole` (commands, REPL, telnet), `StateStore`, `Watchdog`, `HubRegistration`. `Application` keeps the lifecycle and the public API; since 0.4 its members are in `Application::Impl`, with the main loop (`MainLoop`) and the signal thread (`SignalWatcher`) as classes of their own, and `run()` split into prepare, start, loop and shutdown. `StateStore`, `LayoutStore` and `Watchdog` have their own tests. |
| Two control protocols (forge.v1 and `{"cmd": ...}` JSON) | 0.3: the JSON one is counted (`forge.control.legacy_json`), each client is named in a warning once, `[forge].legacy_json = false` refuses it (410). 0.4: both removed; c2 commands on links, the same commands as JSON on `/forge/api/commands` (`/forge/api/tmtc` is the old path). |
| Python clients found the generated modules with `sys.path` searches | `forge` package (`forge.messages_pb2`, `forge.framing`, `forge.c2`, `forge.serial`) installed into site-packages (`FORGE_PYTHON_SITEDIR`: the interpreter's directory for the prefix; Yocto `python3-forge`); applications use `forge_install_python()` for theirs (radio, sa, the scaffolder's). Clients import normally; `forge_msg.py` and every path search are gone. |
| No versions or compatibility rules | 0.3.0 with a CHANGELOG; SOVERSION and `find_package` compatibility follow the minor version until 1.0. `proto/forge/API.json` records the released messages of `proto/forge`; `tools/proto-compat.py` fails CI on renumbered, retyped, renamed or unreserved removed fields and enum values. |
| Restarts, start limits and the hub's view only tested against a fake systemd | CI job `systemd`: forge-hub and radio-controller installed as real units; crash -> restart -> the hub counts it, the start limit -> failed -> the hub says it gave up, the hub's restart and stop buttons. It found two bugs (the Result after an automatic restart is refused, NOTIFY_SOCKET leaking into the hub's children). |
| No Yocto build in CI | `yocto/` is a layer; `yocto/kas/qemuarm64.yml` builds an image from the checkout and boots it with runtime tests; weekly workflow `yocto`, and `yocto-check` (packaging, `bitbake -n`) on every change to the layer. |
| Zynq-7000 (armhf) build not verified | `cmake/toolchains/zynq7000-armhf.cmake`; CI job `cross-armhf` cross-builds everything (host protoc), packages it and runs radio-controller and forgectl under QEMU user emulation. |
| One application could run only once per device in forge-hub | `name@instance`: `--instance` / `$FORGE_INSTANCE` / the template unit's instance; `forge_add_app(... TEMPLATE)` generates `<name>@.service` (config `<instance>.toml`, own StateDirectory); `AppRegistration.instance`; the hub keeps instances apart. |
| Every application's page needed its own port | forge-hub proxies `/apps/<name>/...` (all methods, query strings, redirects kept under the prefix, Server-Sent Events streamed) to the application's port; the dashboard links there (`[proxy].enabled`). forge.js finds the application's root from its own URL (`forge.base`, `forge.url()`); the built-in and example pages use relative URLs. |
| One card layout per page for everybody | With `[auth]`, each signed-in user's layout is their own (kept in the same file under `@users`), the device's layout is the default; `?scope=device` (operator and up) changes the default. Without `[auth]` nothing changes. |
| The radio simulator used 60-70 % of a core at 30.72 MSPS | One pass per 1024-sample block (L1) instead of a pass per step, plain float rotators (std::complex's multiply goes through `__mulsc3`), carriers in 8 independent lanes (vectorized), noise copied in contiguous runs, peak and int16 conversion fused: two channels 64 % -> 35 % of a core (Release). |
| Timer-driven application logic could only be tested by sleeping | `Application::use_manual_clock()` / `advance_time()`, `AppRunner({}, {.manual_time = true})` + `r.advance(d)`: every timer due on the way runs on the main loop in order and reads `now()` == its due time. The scaffolder's tests show it. |
| The threading model was easy to get wrong silently | The dashboard refuses use off the main loop, `invoke()` on the loop runs at once, `background()`, slow-task warnings, docs/threading.md. |
| Long README, no reference, no tutorials | docs/tutorials around a tested example (`examples/pump`), Doxygen API reference, CI artifact. |
| Slow builds of applications | cpp-httplib compiled instead of header-only: `<forge/forge.hpp>` 17.6 -> 8.1 s (-O2); optional PCH for edit-compile cycles. Not done: protobuf 21.12 -> current (needs abseil, a larger change). |
| No C++ API stability rules | docs/API.md; `tools/api-check.py`: exported symbols and configuration keys of a minor series are kept (CI). |
| Docs drifted from the code | A sweep of README, the examples' and Yocto READMEs, header comments, configuration comments and the scaffolder: REP -> ROUTER, `streams()`/`BufferPool` examples, the generated project tree, `[hub]` restart wording, the Yocto application example (generated unit), build paths. Found one bug: spectrum-analyzer's `settle_buffers` default depends on the backend and is now `.computed()`. The defaults check also covers the scaffolder's template. |
