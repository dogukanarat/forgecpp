# Contributing to forgecpp

Thank you for your interest. Bug reports, documentation fixes, examples and
code are all welcome. This page says how to build and test a change and what
a pull request needs.

## Reporting a problem

- **Bugs:** open an issue with the bug report form: what you did, what you
  expected, what happened, and your platform (distribution, compiler, forgecpp
  version or commit). A small program or a `-D` configuration that shows it helps most.
- **Ideas and questions:** open an issue with the feature request form, or start
  a discussion. For larger changes, describe the plan in an issue first so the
  design can be agreed before you write it.
- **Security issues:** do not open a public issue; see [SECURITY.md](SECURITY.md).

## Building and testing

The bundled libraries are built once into a prefix; forgecpp itself is then
built with CMake presets:

```sh
git clone --recursive https://github.com/dogukanarat/forgecpp.git
cd forgecpp
./install.sh --deps-only --prefix $HOME/forgecpp-deps
export FORGECPP_DEPS=$HOME/forgecpp-deps

cmake --workflow --preset dev        # configure, build and test (Debug, build/dev)
tools/check.sh --quick               # format, layers, build, tests
tools/check.sh                       # what CI runs: also fuzzing and the API records
tools/check.sh --install-hook        # the quick checks before every commit
```

Sanitizers, clang-tidy, libFuzzer, the browser tests of the panel, benchmarks
and the release script are described in [docs/development.md](docs/development.md).

## What a pull request needs

1. **One topic.** A bug fix, a feature or a refactoring, not several at once.
2. **Tests.** New behaviour has a test; a bug fix has a test that failed
   before it. Tests must not depend on timing: use
   `forge::testing::AppRunner` with `{.manual_time = true}` and `advance()`
   for timer logic, simulators for hardware.
3. **Checks pass.** `tools/check.sh` (or at least `--quick`) is clean:
   formatting (`.clang-format`; `tools/check.sh --fix` applies it), the layer
   rules, the build without warnings and every test.
4. **Compatibility.** Within a minor version, released messages
   (`proto/forge`), configuration keys and public C++ symbols must not
   change incompatibly; CI checks them (`tools/proto-compat.py`,
   `tools/api-check.py`). The rules are in [docs/API.md](docs/API.md).
5. **Documentation.** User-visible changes update the docs (`docs/`, the
   example READMEs, header comments) and add a line to the `Unreleased`
   section of [CHANGELOG.md](CHANGELOG.md).
6. **Commit messages** in plain English: a short imperative summary line
   ("Fix the serial link reconnect"), a blank line, then what changed and
   why. Keep unrelated formatting changes out of the commit.

## Code style

[docs/CODING_STANDARDS.md](docs/CODING_STANDARDS.md) has the rules. In short:

- C++20, `forge::` namespace, `snake_case` for functions, variables and
  constants, `PascalCase` for types, private members end in `_`.
- The threading rules of [docs/threading.md](docs/threading.md): the main loop
  owns application state; other threads hand work to it.
- Comments say why, in plain English; public headers document what a caller
  needs to know.
- Modules depend only on modules of their own level or lower
  (`tools/check-layers.py`).
- New third-party libraries are git submodules, built as shared libraries by
  `install.sh` and packaged by the Yocto layer; discuss them in an issue first.

## Examples and hardware

Examples run on a PC: anything that needs hardware has a simulator, and the
example's tests use it. If you have run forgecpp on real hardware (GPIO, CAN,
AD936x, the logic analyzer's FPGA core), reports of what worked and what did
not are especially welcome.

## License

By contributing you agree that your contribution is released under the
project's [MIT License](LICENSE).
