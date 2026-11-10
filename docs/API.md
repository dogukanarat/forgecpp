# API stability

What an application may rely on, and how it changes. The checks named here run
in CI on every change.

## What is public

| Surface | Public | Checked by |
|---|---|---|
| C++: `include/forge/*.hpp`, namespace `forge` | yes, except `forge::detail` | `tools/api-check.py` (exported symbols); `examples/`, `examples/pump` (the tutorials) and a freshly scaffolded project must build |
| `forge::detail`, `src/` (`src/app/` services) | no: may change in any release | |
| Messages and command numbers (`proto/forge`), the c2 frame | yes: compatible changes only, always | `tools/proto-compat.py` against `proto/forge/API.json`; the zephyrus compatibility tests (`-DFORGE_ZEPHYRUS_DIR`) |
| Configuration keys the framework declares | yes | `tools/api-check.py` (config keys) |
| HTTP routes under `/forge/api/`, `/health`, `/metrics` | yes | the framework tests |
| `forgectl` commands, application command line flags | yes | the CI smoke test |
| CMake: `forge_add_app`, `forge_add_web`, `forge_add_test`, `forge_add_proto`, `forge_install_python` | yes | examples, scaffolder, CI |
| Python: `forge` package (`framing`, `c2`, `serial`, `messages_pb2`, `hub_pb2`) | yes | CI (the clients) |
| The optional components `forgecpp::sqlite`, `forgecpp::lz4` (`find_package(forgecpp COMPONENTS sqlite lz4)`) | yes | their tests; `tools/api-check.py` records `libforgecpp.so` only, not their libraries |
| Log text, metric names of internals, web page markup | no | |

## Versions

forgecpp uses semantic versions (`CHANGELOG.md`).

- **Before 1.0 (now: 0.4).** Within a minor series (0.4.x) nothing public is
  removed or changed incompatibly: binaries linked against
  `libforgecpp.so.0.4` keep working with every 0.4.x, and sources keep
  compiling. A new minor version (0.5) may break the C++ API; each break is
  listed in the CHANGELOG with what to do instead.
- **From 1.0.** Breaks only in a new major version; deprecated APIs stay at
  least one minor version (`[[deprecated("use X")]]`) before they go.
- **The messages** never break: new fields, messages and commands only,
  removed numbers are reserved, a bound grows only in a request (what
  forgecpp receives) and never changes in a reply or a report (what a peer
  decodes into a struct of the old size; see
  [protocol.md](protocol.md#changing-the-messages)). A device with old and
  new applications keeps working, and so does a zephyrus device on the same
  link.

## Changing the API

- **Add** freely: functions, overloads with new names, fields, keys.
- **Change or remove** within a series: don't. Add the new form, mark the old
  one `[[deprecated("use new_thing()")]]`, and remove it in the next minor
  version with a CHANGELOG entry.
- **At a release**: `tools/proto-compat.py --update` and
  `tools/api-check.py --lib <libforgecpp.so> --app <examples/hello> --update`
  record the API of the new series (`api/forgecpp-<x.y>.json`).

Until 1.0 the record covers exported functions; changes to class layouts,
inline functions and templates are caught by the examples and tests building,
not by a binary comparison.

**What the headers expose.** `forge/application.hpp` no longer parses toml++
(the parser is behind `Config`'s implementation), BS::thread_pool or
moodycamel (`ThreadPool` is a class, `Channel` is forward-declared); the
main-loop waiting code is in the library (`invoke_erased`). The headers that
still carry a third-party library do so on purpose, and say so at the top:
`http.hpp` (cpp-httplib's `Request`/`Response` are the handler types),
`zmq.hpp` (cppzmq), `channel.hpp` / `buffer_pool.hpp` (moodycamel),
`thread_pool.hpp` (BS), `log_rt.hpp` (quill's macros), `json.hpp` (nlohmann,
forgecpp's JSON type everywhere), `pb.hpp` (nanopb's descriptors), `forge.hpp`
(CLI11, for `setup_cli`), `testing.hpp` (doctest).

**Why the library still exports everything.** Hidden visibility
(`-fvisibility=hidden` with an export macro) is the 1.0 step, and it is not
taken yet for a reason: the header-only libraries above keep state in
templates (quill's frontend, moodycamel's thread registry) that the
application and `libforgecpp.so` must share; with hidden visibility each side
gets its own copy, and the two logging frontends or queue registries silently
disagree. The step therefore follows the one that takes those libraries out
of the public headers (a `Logger`-style facade for the pool and the channel),
after which an ABI comparison of the headers (libabigail's `abidiff` against
the previous release's `.so`) joins `tools/check.sh`.
