# Installation

Building forgecpp and its libraries, cross builds, Yocto. The short version is in the [README](../README.md).

Requirements: CMake ≥ 3.16, a C++20 compiler (GCC ≥ 13 / Clang ≥ 17 with libstdc++ 13), git,
libxml2 (for libiio; skip with `--without-iio`). Python 3 with `protobuf`
(the message generator runs at build time; `apt install python3-protobuf`), and
`pyzmq` for the Python clients (optional).

The Python clients (`forge.c2`, `forge.serial`, `forge.framing`, the generated
messages) are installed into site-packages with forgecpp; a machine without
the C++ toolchain (a ground segment, a CI container) installs them from a
build directory or the `--package` tarball with `pip install <build>/python`
(a `pyproject.toml` is written there; package `forge-client`).
Optional: `ninja`, OpenSSL (for HTTPS), zlib (for compression), libusb-1.0 (`--iio-usb`).

```sh
git clone --recursive https://github.com/dogukanarat/forgecpp.git
cd forgecpp
./install.sh                         # installs into /usr/local (uses sudo if needed)
```

One-liner (clones into a temporary directory, installs and cleans up):

```sh
curl -fsSL https://raw.githubusercontent.com/dogukanarat/forgecpp/master/get.sh | bash
curl -fsSL https://raw.githubusercontent.com/dogukanarat/forgecpp/master/get.sh | bash -s -- --prefix /opt/forgecpp --ref v0.4.0
```

`install.sh` builds every dependency as its own CMake project and installs it
into the same prefix (each with its own `find_package` config), then builds
`libforgecpp` on top. SQLite has no CMake build: its amalgamation is made on
the build machine with the checkout's own `configure` (no Tcl needed), then
`cmake/third_party/sqlite` compiles it like the others (`libsqlite3.so.0`,
`sqlite3.h`, `sqlite3.pc` and the `sqlite3` tool, as the distributions
install it). LZ4 brings the `lz4` tool too.

| Option | Description |
|---|---|
| `--prefix DIR` | Install location (default `/usr/local`) |
| `--static` | Static libraries (default: shared: every application on a device uses the same `libforgecpp.so`, `libzmq.so`, `libcpp-httplib.so`, `libiio.so`, `libsqlite3.so` and `liblz4.so`) |
| `--build-type T` | `Release`, `Debug`, `RelWithDebInfo`, `MinSizeRel` |
| `--tests` / `--examples` | Build and run the unit tests / build the examples |
| `--toolchain FILE` | Cross compile (Yocto SDK, OE `toolchain.cmake`) |
| `--destdir DIR` | Stage the install under `DIR` (packaging) |
| `--package [--arch A]` | Produce `forgecpp-<version>-<arch>.tar.gz` without touching the system |
| `--without-ssl`, `--with-ssl`, `--without-zlib` | cpp-httplib features |
| `--without-iio`, `--iio-usb` | Skip libiio, or build libiio's USB backend |
| `--without-hub` | Do not build forge-hub |
| `--without-sqlite`, `--without-lz4` | Skip SQLite or LZ4, and with them the components `forgecpp::sqlite` / `forgecpp::lz4` |
| `--system-protobuf`, `--protoc PATH` | Use the `protoc` on the PATH, or this one (default: built from the bundled sources with `HOST_CC`/`HOST_CXX`; it only runs on the build machine) |
| `--profile embedded` | MinSizeRel and LTO for small targets; the libraries stay shared |
| `--sanitize LIST` | `address,undefined` or `thread`, applied to the dependencies too (use a separate prefix) |
| `--libdir DIR` | e.g. `lib64` instead of `lib` |
| `--skip-deps`, `--deps-only` | Only forgecpp / only the dependencies |
| `--uninstall` | Remove everything that was installed |

See `./install.sh --help` for the full list.

## Yocto / embedded deployment

forgecpp is meant to run on embedded Linux. The whole stack cross-compiles with
a Yocto-style toolchain; on aarch64 the full test suite passes and the example
daemons run with their web UI. Stripped sizes on aarch64: `libforgecpp.so`
≈ 2.1 MB (logging and line editing are compiled into libforgecpp); a scaffolded
application with its embedded web page ≈ 580 KB. Runtime dependencies: the
shared libraries forgecpp installs (libforgecpp, libzmq, libcpp-httplib,
libiio; libsqlite3 and liblz4 for the components), libstdc++/libc, and
OpenSSL/zlib when enabled; with `--static`, only libstdc++/libc.

- **With bitbake:** `yocto/` is a layer that needs nothing but poky
  (oe-core). SQLite, LZ4, OpenSSL, zlib and libxml2 come from oe-core;
  quill, cpp-httplib, concurrentqueue, tomlplusplus and doctest are always
  the layer's; zeromq, cppzmq, nlohmann-json, cli11, libiio, protobuf and
  python3-protobuf come from meta-oe / meta-python, or from the layer's
  `backfill/` when those are not in the build.
  `recipes-support/forgecpp/forgecpp_git.bb` builds forgecpp against them,
  each library a shared library in its own package (the components in
  `forgecpp-sqlite` and `forgecpp-lz4`). Applications then only
  need `DEPENDS = "forgecpp"` and `inherit cmake systemd`; see
  [yocto/README.md](../yocto/README.md).
- **Without bitbake:** with an SDK toolchain,
  `./install.sh --package --prefix /usr --toolchain ...` produces a tar.gz;
  on the target run `tar -xzf forgecpp-*.tar.gz -C /`.

Web pages are compiled into the binaries, so read-only root filesystems work
without extra files.

## Build times

A source file including `<forge/forge.hpp>` takes about 8 s to compile (-O2;
cpp-httplib is a compiled library). For quicker
edit-compile cycles while developing, `-DFORGE_PCH=ON` precompiles the header
for every `forge_add_app` / `forge_add_test` target (one file edited in the
pump example: 17.8 -> 11.9 s); it makes clean builds of small projects slower,
so it is off by default. Include only the headers you need
(`<forge/application.hpp>`, `<forge/web.hpp>`, ...) in files that do not need
everything.

## Updating dependencies

```sh
git -C third_party/quill fetch --tags && git -C third_party/quill checkout v13.x.y
git add third_party/quill && git commit -m "quill: v13.x.y"
```

## Without CMake

`pkg-config --cflags --libs forgecpp` gives the flags for a plain Makefile or
meson (`lib/pkgconfig/forgecpp.pc`): the include directories, cpp-httplib's
feature macros (they change its class layouts, so they must match) and the
libraries. The standard is the consumer's: compile with `-std=c++20` or later.
