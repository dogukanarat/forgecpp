# API stability

What an application may rely on, and how it changes. The checks named here run
in CI on every change.

## What is public

| Surface | Public | Checked by |
|---|---|---|
| C++: `include/forge/*.hpp`, namespace `forge` | yes, except `forge::detail` | `tools/api-check.py` (exported symbols); `examples/`, `examples/pump` (the tutorials) and a freshly scaffolded project must build |
| `forge::detail`, `src/` (`src/app/` services) | no: may change in any release | |
| forge.v1 messages (`proto/forge/v1`) | yes: compatible changes only, always | `tools/proto-compat.py` against `proto/forge/v1/API.json` |
| Configuration keys the framework declares | yes | `tools/api-check.py` (config keys) |
| HTTP routes under `/forge/api/`, `/health`, `/metrics` | yes | the framework tests |
| `forgectl` commands, application command line flags | yes | the CI smoke test |
| CMake: `forge_add_app`, `forge_add_web`, `forge_add_test`, `forge_add_proto`, `forge_install_python` | yes | examples, scaffolder, CI |
| Python: `forge.v1` package | yes | CI (the clients) |
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
- **forge.v1** never breaks: new fields and messages only, removed numbers are
  reserved. A device with old and new applications keeps working.

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
not by a binary comparison. For 1.0: hidden visibility for everything not
public (smaller symbol table, faster loading), and an ABI comparison of the
headers (libabigail) in CI.
