# forgecpp: design fixes plan

Findings of the design review, what is done and the plan for the rest.
Security (authentication defaults, TLS, telnet) is out of scope until the
pre-production security work: forgecpp is used in the lab for now.

## Done

| Finding | Fix |
|---|---|
| Two restart authorities (forge-hub and systemd) | Restarts are systemd's only. `forge_add_app(... RESTART MAX_RESTARTS RESTART_WINDOW RESTART_DELAY WATCHDOG)` generates the unit; forge-hub reads state, `NRestarts`, `Result` and the policy with `systemctl show` and starts / stops / restarts units on request. The hub no longer starts processes or reads their environment. Hangs: systemd's watchdog (the main loop feeds it). |
| Slow, timing-dependent hub tests (60 s) | systemd behind an interface with a fake in tests, 20 ms ticks, sleep processes instead of applications: 2 s. Whole suite: 66 s -> 5 s. |
| Sanitizers only run by hand | CI job `sanitizers`: ASan+UBSan and TSan, the bundled libraries instrumented too, all tests. ccache for every CI job. |
| `invoke()` shared exception objects between threads (TSan) | Errors are described on the main loop and thrown anew on the caller's thread. |
| A stuck main loop hung forgectl and the panel | `invoke_for(timeout, fn)`: forge.v1 and the framework's HTTP routes wait at most `[forge].request_timeout_ms` (2000) for the main loop, then answer `503 main loop busy` with the loop's lag (metric `forge.control.timeouts`); a request that timed out is never applied later. info, health, metrics, logs, audit, trace, alarms and streams do not need the loop. |
| `[forge].control` served one request at a time (REP) | `zmq::Server` is a ROUTER socket with an I/O thread and a worker pool (`[forge].control_workers`, 2); replies go back to the client that asked, requests that waited too long for a worker (or beyond 256 waiting) get 503 without being handled. Clients and messages unchanged. |
| The control service threw an HTTP type (`http::HttpError`) on every channel | `forge::Error{status, message, details}` in `forge/error.hpp`, thrown by `ControlService`, `web::Dashboard`, forge-hub and `invoke_for`; HTTP, forge.v1 and the console map it. `http::HttpError` derives from it (throwing it still works; `catch (const http::HttpError&)` around control calls becomes `catch (const forge::Error&)`). |
| Defaults written twice (schema and every `get_or`) | `Schema::apply_defaults()` fills in every declared key after loading and reloading; declared keys are read with `cfg.value<T>(key)` (framework, forge-hub, all examples migrated; the small examples declare their keys now). Environment-dependent defaults are `.computed()`. A test walks `src/`, `hub/src/`, `examples/` and fails on `get_or` of a declared key. |
| `application.cpp` was 1600 lines doing everything | Internal services in `src/app/` own their state: `HttpFrontend` (server, framework routes, web events and logs), `LayoutStore`, `Messaging` (events, streams, control endpoint), `RemoteConsole` (commands, REPL, telnet), `StateStore`, `Watchdog`, `HubRegistration`. `Application` keeps the lifecycle and the public API (900 lines). `StateStore`, `LayoutStore` and `Watchdog` have their own tests. |
| Two control protocols (forge.v1 and `{"cmd": ...}` JSON) | 0.3: the JSON one is counted (`forge.control.legacy_json`), each client is named in a warning once, `[forge].legacy_json = false` refuses it (410). 0.4: both removed; c2 commands on links, the same commands as JSON on `/forge/api/tmtc`. |
| Python clients found the generated modules with `sys.path` searches | `forge-v1` package (`forge.v1.*_pb2`, `forge.v1.framing`) installed into site-packages (`FORGE_PYTHON_SITEDIR`: the interpreter's directory for the prefix; Yocto `python3-forge-v1`); applications use `forge_install_python()` for theirs (radio.v1, sa.v1, the scaffolder's). Clients import normally; `forge_msg.py` and every path search are gone. |
| No versions or compatibility rules | 0.3.0 with a CHANGELOG; SOVERSION and `find_package` compatibility follow the minor version until 1.0. `proto/forge/v1/API.json` records the released forge.v1 API; `tools/proto-compat.py` fails CI on renumbered, retyped, renamed or unreserved removed fields and enum values. |
| Restarts, start limits and the hub's view only tested against a fake systemd | CI job `systemd`: forge-hub and radio-controller installed as real units; crash -> restart -> the hub counts it, the start limit -> failed -> the hub says it gave up, the hub's restart and stop buttons. It found two bugs (the Result after an automatic restart is refused, NOTIFY_SOCKET leaking into the hub's children). |
| No Yocto build in CI | `yocto/` is a layer; `yocto/kas/qemuarm64.yml` builds an image from the checkout and boots it with runtime tests; nightly workflow `yocto`. |
| Zynq-7000 (armhf) build not verified | `cmake/toolchains/zynq7000-armhf.cmake`; CI job `cross-armhf` cross-builds everything (host protoc), packages it and runs radio-controller and forgectl under QEMU user emulation. |
| One application could run only once per device in forge-hub | `name@instance`: `--instance` / `$FORGE_INSTANCE` / the template unit's instance; `forge_add_app(... TEMPLATE)` generates `<name>@.service` (config `<instance>.toml`, own StateDirectory); `AppRegistration.instance`; the hub keeps instances apart. |
| Every application's page needed its own port | forge-hub proxies `/apps/<name>/...` (all methods, query strings, redirects kept under the prefix, Server-Sent Events streamed) to the application's port; the dashboard links there (`[proxy].enabled`). forge.js finds the application's root from its own URL (`forge.base`, `forge.url()`); the built-in and example pages use relative URLs. |
| One card layout per page for everybody | With `[auth]`, each signed-in user's layout is their own (kept in the same file under `@users`), the device's layout is the default; `?scope=device` (operator and up) changes the default. Without `[auth]` nothing changes. |
| The radio simulator used 60-70 % of a core at 30.72 MSPS | One pass per 1024-sample block (L1) instead of a pass per step, plain float rotators (std::complex's multiply goes through `__mulsc3`), carriers in 8 independent lanes (vectorized), noise copied in contiguous runs, peak and int16 conversion fused: two channels 64 % -> 35 % of a core (Release). |
| Timer-driven application logic could only be tested by sleeping | `Application::use_manual_clock()` / `advance_time()`, `AppRunner({}, {.manual_time = true})` + `r.advance(d)`: every timer due on the way runs on the main loop in order and reads `now()` == its due time. The scaffolder's tests show it. |
| The threading model was easy to get wrong silently | The dashboard refuses use off the main loop, `call()` on the loop runs at once, `background()`, slow-task warnings, docs/threading.md. |
| Long README, no reference, no tutorials | docs/tutorials around a tested example (`examples/pump`), Doxygen API reference, CI artifact. |
| Slow builds of applications | cpp-httplib compiled instead of header-only: `<forge/forge.hpp>` 17.6 -> 8.1 s (-O2); optional PCH for edit-compile cycles. Not done: protobuf 21.12 -> current (needs abseil, a larger change). |
| No C++ API stability rules | docs/API.md; `tools/api-check.py`: exported symbols and configuration keys of a minor series are kept (CI). |
| Docs drifted from the code | A sweep of README, the examples' and Yocto READMEs, header comments, configuration comments and the scaffolder: REP -> ROUTER, `streams()`/`BufferPool` examples, the generated project tree, `[hub]` restart wording, the Yocto application example (generated unit), build paths. Found one bug: spectrum-analyzer's `settle_buffers` default depends on the backend and is now `.computed()`. The defaults check also covers the scaffolder's template. |

## Done: the design review of 0.4.0

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

Left for after 0.4.0, with the reason:

| Item | Why later |
|---|---|
| Hidden symbol visibility with an export macro | The header-only libraries in the public headers keep state in templates (quill's frontend, moodycamel's thread registry); hidden visibility would give the application and the library each their own copy. First the facades (pool, channel), then visibility and `abidiff` ([API.md](API.md)). |
| `Application` data members behind an `Impl` | The services in `src/app/` reach a few private members through `friend`; the header lost its heavy includes and the `invoke()` machinery in this round, which was the compile-time part. The layout move is a 0.5 item. |
| `forge::Remote` finding a program through forge-hub | Needs a hub command that lists registrations with their endpoints; the port plan in field-notes.md covers today's need. |
| A CMake superbuild (or vcpkg/conan) replacing `install.sh`'s dependency build | The Yocto layer no longer needs it; a host install still uses `install.sh`, which works. |
| Running `yocto-check-layer` and a bitbake build of the new recipes | The recipes were written against the scarthgap classes and reviewed, not built here; the nightly kas job is the place. |

## Plan

Ordered by risk to the running system. Each step lands on its own with tests,
without changing behaviour that is not named.

## Framework plan (0.4.0)

Agreed direction for the framework itself (the examples follow it):

1. **C++20** for the library, its exported targets and the examples. Every
   supported toolchain has it: Yocto scarthgap GCC 13.5, Ubuntu 24.04 GCC 13/14
   and clang 18, the armhf cross GCC 13.
2. **linenoise** (antirez) instead of replxx (unmaintained since 2021): completion,
   hints, history, UTF-8, log output while the user types (hide/show,
   non-blocking API). Input syntax colouring is dropped.
3. **Quill** instead of spdlog, behind a forgecpp log facade (applications
   no longer see the backend): asynchronous, bounded dropping queues on
   real-time threads, crash flush. Coloured logs on the terminal, the
   interactive console and telnet (ANSI), plain with level prefixes under
   journald, level colours (CSS) on the web panel and forge-hub. fmt is
   replaced by `std::format`.
4. Done, differently from the first plan: **messages stay .proto**, the
   runtime is **nanopb with static structs** (every string, bytes and
   repeated field bounded: no allocation, the same file builds for
   zephyrus), names and JSON from descriptor sets (`tools/forge-proto.py`).
   forge.v1 is replaced by **c2 + tmtc**: commands are request/reply pairs
   with numbers, sent as zephyrus' c2 frames on links and as JSON on the web,
   one handler for both. `tools/proto-compat.py` stays (it also checks
   bounds) instead of Buf.
5. Done: `forge::Worker` on `std::jthread` and `Application::worker()`,
   `Sender::send_wait()`, atomics and metrics bound to the panel
   (`g.value(id, label, counter)`), `background()` with an error path,
   Dependabot for submodules and actions, clang-tidy (`tools/check.sh
   --tidy`), fuzzing (`tests/fuzz`), hardened systemd units. Typed
   configuration was already there (`Schema` + `cfg.value<T>()`, checked at
   start and reload); the I/O helpers that were missing came with the serial
   link (`serial::Port`, `serial::Link`). Also done: the **serial link**
   (c2 on a UART with zephyrus's COBS framing, checked against zephyrus's C
   code), typed `get`/`set` values, the module of a refusal (`app` for the
   application's commands), local tooling instead of CI (`tools/check.sh`,
   pre-commit hook, presets, `tools/release.sh`, `forgecpp_bench`).
6. Later: Asio-based I/O, WebSocket server, connector modules (MQTT, Modbus,
   CAN with ISO-TP for c2, GPIO, OPC UA), Python bindings. Not planned: more
   than two `commands` entries per reply page (each entry is about 6 kB;
   a larger page would not fit a zephyrus peer's default buffer).

## Hardware bring-up plan (ZC702 / Z7020 + FMCOMMS5) — when the board arrives

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
two AD9361). libiio 0.26 comes from meta-oe.

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

## Pre-production security work (later)

Secure defaults (no `[auth]`: control, telnet and the `[forge]` endpoints only on
localhost), hub registration over a Unix socket with `SO_PEERCRED`, TLS (or a
terminating proxy) for HTTP, telnet replaced or tunnelled through ssh, a review
of the roles of every command, and a role for commands on a link (c2 frames
carry no credentials; today they get `[auth].default_role`).
