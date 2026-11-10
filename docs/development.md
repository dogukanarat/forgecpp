# Working on forgecpp

Checks, presets, versions and the layout of the repository.

```sh
export FORGECPP_DEPS=$HOME/forgecpp-deps     # once: ./install.sh --deps-only --prefix $FORGECPP_DEPS
cmake --workflow --preset dev                # configure, build and test (Debug, build/dev)
tools/check.sh                               # what CI checks: format, layers, build, tests, fuzz, API records
tools/check.sh --quick                       # the same without fuzz and the compatibility records
tools/check.sh --fix                         # apply .clang-format
tools/check.sh --asan                        # also AddressSanitizer + UBSan
tools/check.sh --tidy                        # clang-tidy on what changed since origin/master
tools/check.sh --install-hook                # quick checks before every commit, blame skips formatting commits
```

The panel has browser tests (`tests/web/panel.test.js`, against
`tests/web/panel_app.cpp`); ctest runs them when `node` can load Playwright
(`npm install -g playwright && npx playwright install chromium`).

Presets: `dev`, `release`, `asan`, `tsan`, `fuzz` (libFuzzer, clang),
`armhf` (`cmake --list-presets`). The fuzz targets (`tests/fuzz`) also run
as short tests; a failing input is saved as `fuzz-failure.bin` and can be
replayed: `./tests/fuzz_pb fuzz-failure.bin`.

`build/tests/forgecpp_bench` (Release) measures the message paths: protobuf
encode and decode, c2 dispatch, the serial framing, a command round trip and
a stream on ZeroMQ. A name argument runs only the cases that start with it
(`forgecpp_bench serial`). Use it to compare builds and machines.

## Versions

Semantic versions, changes in [CHANGELOG.md](../CHANGELOG.md). Until 1.0 a minor
version may change the C++ API: `libforgecpp.so.<major>.<minor>` and
`find_package(forgecpp 0.4)` accepts 0.4.x only. The messages only change
compatibly (new fields, messages and commands; removed numbers reserved;
bounds only grow): CI checks them against the last release's API
(`proto/forge/API.json`) with `tools/proto-compat.py`, which also records it
at a release (`--update`). Within a minor
series the C++ API and the configuration keys stay compatible too
(`tools/api-check.py`, `api/forgecpp-<x.y>.json`); what counts as public and
how it may change: [docs/API.md](API.md).

A release: `tools/release.sh 0.4.0` on master with a clean tree. It refuses
when the version in `CMakeLists.txt` or the `## Unreleased (0.4.0)` section of
the CHANGELOG does not match, runs `tools/check.sh`, records both APIs, dates
the CHANGELOG, commits "Release 0.4.0" and tags `v0.4.0`. Pushing is yours:
`git push origin master v0.4.0`. `--dry-run` stops after the checks.

## Directory layout

```
include/forge/     public headers (forge.hpp includes everything)
proto/forge/       forgecpp's commands and messages (messages.proto, hub.proto)
src/               libforgecpp sources
src/web/           built-in control panel, forge.js, forge.css (embedded in the library)
third_party/       submodules
cmake/             package config, pkg-config, forge_add_app/forge_add_web/forge_add_test
tools/             forgecpp-new (project scaffolder), forgectl, forge-proto.py (message generator)
hub/               forge-hub: device dashboard and registry (a product, not an example)
examples/          hello, sensor-service, pipeline (multi-threaded), three-threads,
                   thermostat (web site),
                   radio-controller (AD9364 over libiio + ZeroMQ),
                   spectrum-analyzer (AD936x spectrum analyzer), qam-modem (16QAM over
                   radio-controller), channel-emulator, common/ad936x, common/channel
tests/             doctest unit tests
yocto/             bitbake recipes (forgecpp, radio-controller, spectrum-analyzer, qam-modem,
                   channel-emulator)
install.sh, get.sh installation
```
