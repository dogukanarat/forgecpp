#!/usr/bin/env bash
# forgecpp installer
#
# Builds every bundled library from third_party/ (git submodules) as its own
# CMake project, installs them into one prefix, then builds and installs
# libforgecpp on top. After that any project can simply do:
#
#     find_package(forgecpp REQUIRED)
#     target_link_libraries(myapp PRIVATE forgecpp::forgecpp)
#
# Run ./install.sh --help for all options.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${SCRIPT_DIR}"
TP_DIR="${SRC_DIR}/third_party"

# ---------------------------------------------------------------------------
# defaults
# ---------------------------------------------------------------------------
PREFIX="/usr/local"
BUILD_DIR="${SRC_DIR}/build"
BUILD_TYPE="Release"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"
SHARED="ON"
TOOLCHAIN=""
DESTDIR_ARG="${DESTDIR:-}"
LIBDIR=""
WITH_SSL="auto"
WITH_ZLIB="auto"
WITH_IIO="ON"
WITH_HUB="ON"
SYSTEM_PROTOBUF="OFF"
HOST_PROTOC=""
PROFILE=""
SANITIZE=""
IIO_USB="OFF"
BUILD_TESTS="OFF"
RUN_TESTS="OFF"
BUILD_EXAMPLES="OFF"
PACKAGE="OFF"
ARCH=""
RPATH="auto"
GENERATOR=""
EXTRA_CMAKE_ARGS=()
ACTION="install"
SKIP_DEPS="OFF"
DEPS_ONLY="OFF"
USE_SUDO="auto"
QUIET="OFF"

# name | source dir | extra cmake args
DEPS=(quill CLI11 concurrentqueue json tomlplusplus cpp-httplib doctest libzmq cppzmq protobuf libiio)

usage() {
    cat <<EOF
forgecpp installer

Usage: ./install.sh [options]

Install location
  --prefix DIR          Install prefix (default: ${PREFIX})
  --libdir DIR          Library dir relative to prefix (default: lib; Yocto: \${baselib})
  --destdir DIR         Stage the install under DIR (packaging, Yocto do_install)
  --build-dir DIR       Build directory (default: ./build)

Build
  --build-type TYPE     Release | Debug | RelWithDebInfo | MinSizeRel (default: ${BUILD_TYPE})
  -j, --jobs N          Parallel jobs (default: ${JOBS})
  --static              Build static libraries (default: shared)
  --toolchain FILE      CMake toolchain file for cross compiling (Yocto SDK, OE, ...)
  --generator GEN       CMake generator (default: Ninja if available)
  --without-ssl         Build cpp-httplib without OpenSSL (no HTTPS)
  --with-ssl            Require OpenSSL (fail if missing)
  --without-zlib        Build cpp-httplib without zlib compression
  --system-protobuf     Use the system's protobuf (libprotobuf-dev + protoc) instead of the bundled one
  --protoc PATH         protoc for the build machine (cross builds; default: built from the bundled
                        protobuf with HOST_CC / HOST_CXX, or cc / c++)
  --profile embedded    Size/speed settings for small targets: MinSizeRel, LTO, static, no tests
  --sanitize LIST       Build everything with sanitizers: address,undefined or thread (debugging;
                        use a separate --prefix, the libraries are instrumented too)
  --without-iio         Skip libiio (the AD9364 examples then only have their simulator)
  --without-hub         Do not build forge-hub (the dashboard of the device's applications)
  --iio-usb             Build libiio's USB backend (needs libusb-1.0)
  --no-rpath            Do not set \$ORIGIN rpaths on installed libraries
  --cmake-arg ARG       Extra argument passed to every CMake configure (repeatable)

Extras
  --tests               Build and run the forgecpp unit tests
  --examples            Build the example applications (in the build dir)
  --package             Do not touch the system; stage into the build dir and
                        create forgecpp-<version>-<arch>.tar.gz (extract with tar -C /)
  --arch NAME           Architecture name used in the package file name
  --skip-deps           Only (re)build forgecpp itself, deps are already installed
  --deps-only           Only build and install the third-party libraries
  --sudo / --no-sudo    Force / forbid sudo for the install step (default: auto)
  -q, --quiet           Less build output

Other actions
  --uninstall           Remove everything a previous install put on the system
  --clean               Remove the build directory
  -h, --help            Show this help

Examples
  ./install.sh                                  # build + install into /usr/local
  ./install.sh --prefix /opt/forgecpp --tests   # custom prefix, run tests
  ./install.sh --prefix \$HOME/.local            # no root needed
  ./install.sh --package --arch aarch64 --toolchain /path/to/toolchain.cmake --prefix /usr
EOF
}

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
if [[ -t 1 ]]; then
    C_BOLD=$'\033[1m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_RED=$'\033[31m'; C_RESET=$'\033[0m'
else
    C_BOLD=""; C_GREEN=""; C_YELLOW=""; C_RED=""; C_RESET=""
fi
info()  { echo "${C_BOLD}${C_GREEN}==>${C_RESET}${C_BOLD} $*${C_RESET}"; }
warn()  { echo "${C_BOLD}${C_YELLOW}warning:${C_RESET} $*" >&2; }
die()   { echo "${C_BOLD}${C_RED}error:${C_RESET} $*" >&2; exit 1; }

need_arg() { [[ $# -ge 2 && -n "$2" ]] || die "option $1 needs an argument"; }

abspath() {
    local p="$1"
    if [[ "$p" != /* ]]; then p="$(pwd)/$p"; fi
    echo "$p"
}

version_ge() { [[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" == "$2" ]]; }

# ---------------------------------------------------------------------------
# argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)       need_arg "$@"; PREFIX="$2"; shift 2 ;;
        --prefix=*)     PREFIX="${1#*=}"; shift ;;
        --libdir)       need_arg "$@"; LIBDIR="$2"; shift 2 ;;
        --libdir=*)     LIBDIR="${1#*=}"; shift ;;
        --destdir)      need_arg "$@"; DESTDIR_ARG="$2"; shift 2 ;;
        --destdir=*)    DESTDIR_ARG="${1#*=}"; shift ;;
        --build-dir)    need_arg "$@"; BUILD_DIR="$2"; shift 2 ;;
        --build-dir=*)  BUILD_DIR="${1#*=}"; shift ;;
        --build-type)   need_arg "$@"; BUILD_TYPE="$2"; shift 2 ;;
        --build-type=*) BUILD_TYPE="${1#*=}"; shift ;;
        -j|--jobs)      need_arg "$@"; JOBS="$2"; shift 2 ;;
        -j*)            JOBS="${1#-j}"; shift ;;
        --jobs=*)       JOBS="${1#*=}"; shift ;;
        --static)       SHARED="OFF"; shift ;;
        --shared)       SHARED="ON"; shift ;;
        --toolchain)    need_arg "$@"; TOOLCHAIN="$2"; shift 2 ;;
        --toolchain=*)  TOOLCHAIN="${1#*=}"; shift ;;
        --generator)    need_arg "$@"; GENERATOR="$2"; shift 2 ;;
        --generator=*)  GENERATOR="${1#*=}"; shift ;;
        --without-ssl)  WITH_SSL="OFF"; shift ;;
        --with-ssl)     WITH_SSL="ON"; shift ;;
        --without-zlib) WITH_ZLIB="OFF"; shift ;;
        --without-zmq)  die "--without-zmq: ZeroMQ is part of forgecpp now (the forge.v1 control and streams use it)" ;;
        --system-protobuf) SYSTEM_PROTOBUF="ON"; shift ;;
        --protoc)       need_arg "$@"; HOST_PROTOC="$2"; shift 2 ;;
        --protoc=*)     HOST_PROTOC="${1#*=}"; shift ;;
        --profile)      need_arg "$@"; PROFILE="$2"; shift 2 ;;
        --sanitize)     need_arg "$@"; SANITIZE="$2"; shift 2 ;;
        --profile=*)    PROFILE="${1#*=}"; shift ;;
        --without-iio)  WITH_IIO="OFF"; shift ;;
        --without-hub)  WITH_HUB="OFF"; shift ;;
        --iio-usb)      IIO_USB="ON"; shift ;;
        --with-zlib)    WITH_ZLIB="ON"; shift ;;
        --no-rpath)     RPATH="OFF"; shift ;;
        --rpath)        RPATH="ON"; shift ;;
        --cmake-arg)    need_arg "$@"; EXTRA_CMAKE_ARGS+=("$2"); shift 2 ;;
        --cmake-arg=*)  EXTRA_CMAKE_ARGS+=("${1#*=}"); shift ;;
        --tests)        BUILD_TESTS="ON"; RUN_TESTS="ON"; shift ;;
        --examples)     BUILD_EXAMPLES="ON"; shift ;;
        --package)      PACKAGE="ON"; shift ;;
        --arch)         need_arg "$@"; ARCH="$2"; shift 2 ;;
        --arch=*)       ARCH="${1#*=}"; shift ;;
        --skip-deps)    SKIP_DEPS="ON"; shift ;;
        --deps-only)    DEPS_ONLY="ON"; shift ;;
        --sudo)         USE_SUDO="ON"; shift ;;
        --no-sudo)      USE_SUDO="OFF"; shift ;;
        -q|--quiet)     QUIET="ON"; shift ;;
        --uninstall)    ACTION="uninstall"; shift ;;
        --clean)        ACTION="clean"; shift ;;
        -h|--help)      usage; exit 0 ;;
        *)              die "unknown option '$1' (see --help)" ;;
    esac
done

case "${PROFILE}" in
    "") ;;
    embedded)
        # Small targets (e.g. Zynq-7000): compact, link-time optimized, self-contained binaries.
        BUILD_TYPE="MinSizeRel"
        SHARED="OFF"
        EXTRA_CMAKE_ARGS+=("-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON")
        ;;
    *) die "unknown profile '${PROFILE}' (embedded)" ;;
esac

BUILD_DIR="$(abspath "${BUILD_DIR}")"
[[ "${PREFIX}" == /* ]] || PREFIX="$(abspath "${PREFIX}")"
[[ -z "${DESTDIR_ARG}" ]] || DESTDIR_ARG="$(abspath "${DESTDIR_ARG}")"
[[ -z "${TOOLCHAIN}" ]] || TOOLCHAIN="$(abspath "${TOOLCHAIN}")"
PREFIX="${PREFIX%/}"; [[ -n "${PREFIX}" ]] || PREFIX="/"

FORGECPP_VERSION="$(sed -n 's/^ *VERSION \([0-9.]*\)$/\1/p' "${SRC_DIR}/CMakeLists.txt" | head -n1)"

# ---------------------------------------------------------------------------
# clean / uninstall
# ---------------------------------------------------------------------------
if [[ "${ACTION}" == "clean" ]]; then
    info "Removing ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
    exit 0
fi

pick_sudo() {
    # $1 = directory that will be written to
    SUDO=""
    case "${USE_SUDO}" in
        ON)  SUDO="sudo" ;;
        OFF) SUDO="" ;;
        auto)
            local dir="$1"
            while [[ ! -e "${dir}" && "${dir}" != "/" ]]; do dir="$(dirname "${dir}")"; done
            if [[ ! -w "${dir}" && "$(id -u)" -ne 0 ]]; then
                command -v sudo >/dev/null 2>&1 || die "no write access to ${dir} and sudo is not available (use --prefix)"
                SUDO="sudo"
            fi
            ;;
    esac
}

if [[ "${ACTION}" == "uninstall" ]]; then
    shopt -s nullglob
    manifests=("${BUILD_DIR}"/*/install_manifest.txt)
    SAVED_MANIFEST="${PREFIX}/share/forgecpp/install_manifest.txt"
    if [[ -f "${SAVED_MANIFEST}" ]]; then manifests+=("${SAVED_MANIFEST}"); fi
    [[ ${#manifests[@]} -gt 0 ]] || die "no install manifest found in ${BUILD_DIR} or ${SAVED_MANIFEST} (nothing to uninstall)"
    pick_sudo "${PREFIX}"
    info "Uninstalling files listed in ${#manifests[@]} manifest(s)"
    for m in "${manifests[@]}"; do
        while IFS= read -r f || [[ -n "$f" ]]; do
            [[ -n "$f" ]] || continue
            if [[ -e "$f" || -L "$f" ]]; then ${SUDO} rm -f "$f"; fi
        done < "$m"
    done
    if [[ -f "${SAVED_MANIFEST}" ]]; then ${SUDO} rm -f "${SAVED_MANIFEST}"; fi
    # Remove now-empty directories we created.
    for d in "${PREFIX}/include/forge" "${PREFIX}/include/nlohmann" "${PREFIX}/include/CLI" \
             "${PREFIX}/include/quill" "${PREFIX}/include/toml++" "${PREFIX}/include/doctest" \
             "${PREFIX}/include/concurrentqueue" "${PREFIX}"/lib*/cmake/forgecpp \
             "${PREFIX}/share/doc/forgecpp" "${PREFIX}/share/forgecpp"; do
        [[ -d "$d" ]] && ${SUDO} find "$d" -depth -type d -empty -delete 2>/dev/null || true
    done
    info "forgecpp uninstalled from ${PREFIX}"
    exit 0
fi

# ---------------------------------------------------------------------------
# sanity checks
# ---------------------------------------------------------------------------
command -v cmake >/dev/null 2>&1 || die "cmake not found (need >= 3.16)"
CMAKE_VERSION="$(cmake --version | head -n1 | awk '{print $3}')"
version_ge "${CMAKE_VERSION}" "3.16" || die "cmake ${CMAKE_VERSION} is too old (need >= 3.16)"

if [[ -z "${TOOLCHAIN}" ]]; then
    CXX_BIN="${CXX:-}"
    if [[ -z "${CXX_BIN}" ]]; then
        for c in c++ g++ clang++; do
            if command -v "$c" >/dev/null 2>&1; then CXX_BIN="$c"; break; fi
        done
    fi
    [[ -n "${CXX_BIN}" ]] || die "no C++ compiler found (install g++ or clang++, or set CXX)"
else
    [[ -f "${TOOLCHAIN}" ]] || die "toolchain file not found: ${TOOLCHAIN}"
    [[ -n "${DESTDIR_ARG}" || "${PACKAGE}" == "ON" ]] || \
        warn "cross compiling without --destdir/--package installs straight into ${PREFIX} on this machine"
fi

if [[ -z "${GENERATOR}" ]] && command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
fi

# Submodules
missing=()
for d in "${DEPS[@]}" thread-pool; do
    if [[ ! -f "${TP_DIR}/${d}/CMakeLists.txt" && ! -f "${TP_DIR}/${d}/include/BS_thread_pool.hpp" ]]; then
        missing+=("$d")
    fi
done
if [[ ${#missing[@]} -gt 0 ]]; then
    if [[ -e "${SRC_DIR}/.git" ]] && command -v git >/dev/null 2>&1; then
        info "Fetching git submodules (${missing[*]})"
        git -C "${SRC_DIR}" submodule update --init --recursive --depth 1 || \
            git -C "${SRC_DIR}" submodule update --init --recursive
    else
        die "third-party sources missing (${missing[*]}). Clone with --recursive or use a release tarball."
    fi
fi

# ---------------------------------------------------------------------------
# install layout
# ---------------------------------------------------------------------------
if [[ "${PACKAGE}" == "ON" && -z "${DESTDIR_ARG}" ]]; then
    DESTDIR_ARG="${BUILD_DIR}/stage"
    rm -rf "${DESTDIR_ARG}"
fi
STAGE="${DESTDIR_ARG}${PREFIX}"
STAGE="${STAGE%/}"

if [[ "${RPATH}" == "auto" ]]; then
    if [[ "${PREFIX}" == "/usr" ]]; then RPATH="OFF"; else RPATH="ON"; fi
fi

SUDO=""
if [[ -z "${DESTDIR_ARG}" ]]; then
    pick_sudo "${PREFIX}"
fi

mkdir -p "${BUILD_DIR}"

# Cross builds: wrap the user's toolchain so find_package() also looks in the stage.
EFFECTIVE_TOOLCHAIN="${TOOLCHAIN}"
if [[ -n "${TOOLCHAIN}" && -n "${DESTDIR_ARG}" ]]; then
    EFFECTIVE_TOOLCHAIN="${BUILD_DIR}/forgecpp-toolchain.cmake"
    cat > "${EFFECTIVE_TOOLCHAIN}" <<EOF
# Generated by forgecpp install.sh
include("${TOOLCHAIN}")
list(APPEND CMAKE_FIND_ROOT_PATH "${DESTDIR_ARG}")
EOF
fi

COMMON_ARGS=(
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
    "-DCMAKE_INSTALL_PREFIX=${PREFIX}"
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
    "-DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON"
    "-DBUILD_TESTING=OFF"
)
[[ -n "${LIBDIR}" ]] && COMMON_ARGS+=("-DCMAKE_INSTALL_LIBDIR=${LIBDIR}")
[[ -n "${GENERATOR}" ]] && COMMON_ARGS+=("-G" "${GENERATOR}")
if [[ -n "${EFFECTIVE_TOOLCHAIN}" ]]; then
    COMMON_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=${EFFECTIVE_TOOLCHAIN}" "-DCMAKE_PREFIX_PATH=${PREFIX}")
else
    COMMON_ARGS+=("-DCMAKE_PREFIX_PATH=${STAGE}")
fi
if [[ "${RPATH}" == "ON" ]]; then
    # Libraries find each other next to themselves, programs in bin/ one level up.
    COMMON_ARGS+=("-DCMAKE_INSTALL_RPATH=\$ORIGIN;\$ORIGIN/../${LIBDIR:-lib}")
fi
if [[ -n "${SANITIZE}" ]]; then
    # Instrument the dependencies as well (ThreadSanitizer needs it to see their locks).
    SAN_FLAGS="-fsanitize=${SANITIZE} -fno-omit-frame-pointer -g"
    COMMON_ARGS+=("-DCMAKE_C_FLAGS=${SAN_FLAGS}" "-DCMAKE_CXX_FLAGS=${SAN_FLAGS}"
                  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=${SANITIZE}"
                  "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=${SANITIZE}")
fi
COMMON_ARGS+=("${EXTRA_CMAKE_ARGS[@]+"${EXTRA_CMAKE_ARGS[@]}"}")

dep_args() {
    case "$1" in
        quill)
            # header-only: installs the headers and quillConfig.cmake
            echo "-DQUILL_BUILD_TESTS=OFF -DQUILL_BUILD_EXAMPLES=OFF -DQUILL_BUILD_BENCHMARKS=OFF" ;;
        CLI11)
            echo "-DCLI11_BUILD_TESTS=OFF -DCLI11_BUILD_EXAMPLES=OFF -DCLI11_BUILD_DOCS=OFF -DCLI11_PRECOMPILED=OFF" ;;
        json)
            echo "-DJSON_BuildTests=OFF -DJSON_Install=ON" ;;
        tomlplusplus)
            echo "-DBUILD_EXAMPLES=OFF" ;;
        cpp-httplib)
            # Compiled, not header-only: its function bodies are built once into
            # libcpp-httplib instead of in every file that includes forge/http.hpp
            # (a file including forge/forge.hpp: 17.6 -> 8.1 s at -O2).
            echo "-DHTTPLIB_COMPILE=ON -DHTTPLIB_SHARED=${SHARED} -DHTTPLIB_TEST=OFF -DHTTPLIB_INSTALL=ON"
            echo "-DHTTPLIB_USE_BROTLI_IF_AVAILABLE=OFF -DHTTPLIB_USE_ZSTD_IF_AVAILABLE=OFF"
            case "${WITH_SSL}" in
                ON)  echo "-DHTTPLIB_REQUIRE_OPENSSL=ON" ;;
                OFF) echo "-DHTTPLIB_USE_OPENSSL_IF_AVAILABLE=OFF" ;;
                *)   echo "-DHTTPLIB_USE_OPENSSL_IF_AVAILABLE=ON" ;;
            esac
            case "${WITH_ZLIB}" in
                ON)  echo "-DHTTPLIB_REQUIRE_ZLIB=ON" ;;
                OFF) echo "-DHTTPLIB_USE_ZLIB_IF_AVAILABLE=OFF" ;;
                *)   echo "-DHTTPLIB_USE_ZLIB_IF_AVAILABLE=ON" ;;
            esac ;;
        libzmq)
            echo "-DBUILD_TESTS=OFF -DWITH_PERF_TOOL=OFF -DWITH_DOCS=OFF -DENABLE_DRAFTS=OFF"
            echo "-DENABLE_PRECOMPILED=OFF -DWITH_LIBSODIUM=OFF -DENABLE_CURVE=OFF -DWITH_LIBBSD=OFF"
            if [[ "${SHARED}" == "ON" ]]; then echo "-DBUILD_SHARED=ON -DBUILD_STATIC=OFF"
            else echo "-DBUILD_SHARED=OFF -DBUILD_STATIC=ON"; fi ;;
        cppzmq)
            echo "-DCPPZMQ_BUILD_TESTS=OFF" ;;
        protobuf)
            # Cross builds use a protoc built for the build machine (see build_host_protoc).
            echo "-Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_EXAMPLES=OFF -Dprotobuf_WITH_ZLIB=OFF"
            echo "-Dprotobuf_BUILD_SHARED_LIBS=${SHARED} -Dprotobuf_INSTALL=ON -Dprotobuf_BUILD_LIBPROTOC=ON"
            if [[ -n "${TOOLCHAIN}" ]]; then echo "-Dprotobuf_BUILD_PROTOC_BINARIES=OFF"
            else echo "-Dprotobuf_BUILD_PROTOC_BINARIES=ON"; fi ;;
        libiio)
            # Local (in-kernel IIO) + network (iiod, e.g. ip:192.168.2.1) + XML backends.
            echo "-DBUILD_SHARED_LIBS=${SHARED} -DWITH_IIOD=OFF -DWITH_TESTS=ON -DWITH_EXAMPLES=OFF"
            echo "-DWITH_DOC=OFF -DWITH_MAN=OFF -DPYTHON_BINDINGS=OFF -DCSHARP_BINDINGS=OFF -DCPP_BINDINGS=OFF"
            echo "-DHAVE_DNS_SD=OFF -DWITH_SERIAL_BACKEND=OFF -DINSTALL_UDEV_RULE=OFF -DWITH_ZSTD=OFF"
            echo "-DWITH_USB_BACKEND=${IIO_USB} -DOSX_PACKAGE=OFF" ;;
        doctest)
            echo "-DDOCTEST_WITH_TESTS=OFF -DDOCTEST_WITH_MAIN_IN_STATIC_LIB=ON" ;;
        *) ;;
    esac
}

LOG_DIR="${BUILD_DIR}/logs"
mkdir -p "${LOG_DIR}"

run_logged() {
    # run_logged <logfile> <cmd...>
    local logfile="$1"; shift
    if [[ "${QUIET}" == "ON" ]]; then
        if ! "$@" >"${logfile}" 2>&1; then
            tail -n 40 "${logfile}" >&2
            die "command failed: $* (full log: ${logfile})"
        fi
    else
        if ! "$@" 2>&1 | tee "${logfile}"; then
            die "command failed: $* (full log: ${logfile})"
        fi
    fi
}

do_install() {
    # do_install <build dir> <name>
    if [[ -n "${DESTDIR_ARG}" ]]; then
        DESTDIR="${DESTDIR_ARG}" run_logged "${LOG_DIR}/$2-install.log" cmake --install "$1"
    else
        run_logged "${LOG_DIR}/$2-install.log" ${SUDO} cmake --install "$1"
    fi
}

build_project() {
    # build_project <name> <source dir> [extra cmake args...]
    local name="$1" src="$2"; shift 2
    local bdir="${BUILD_DIR}/${name}"
    info "[${name}] configure"
    run_logged "${LOG_DIR}/${name}-configure.log" \
        cmake -S "${src}" -B "${bdir}" "${COMMON_ARGS[@]}" "$@"
    info "[${name}] build"
    run_logged "${LOG_DIR}/${name}-build.log" cmake --build "${bdir}" --parallel "${JOBS}"
    info "[${name}] install"
    do_install "${bdir}" "${name}"
}

# ---------------------------------------------------------------------------
# go
# ---------------------------------------------------------------------------
info "forgecpp ${FORGECPP_VERSION}"
echo "    prefix      : ${PREFIX}"
[[ -n "${DESTDIR_ARG}" ]] && echo "    destdir     : ${DESTDIR_ARG}"
echo "    build dir   : ${BUILD_DIR}"
echo "    build type  : ${BUILD_TYPE}"
echo "    libraries   : $([[ ${SHARED} == ON ]] && echo shared || echo static)"
[[ -n "${TOOLCHAIN}" ]] && echo "    toolchain   : ${TOOLCHAIN}"
[[ -n "${SUDO}" ]] && echo "    install via : sudo"
echo

# protoc must run on the build machine. Native builds use the protoc they build;
# cross builds get one built from the same protobuf sources for the host (the
# generated code has to match the target's libprotobuf version exactly).
build_host_protoc() {
    local bdir="${BUILD_DIR}/host-protobuf"
    local prefix="${BUILD_DIR}/host-tools"
    if [[ -x "${prefix}/bin/protoc" ]]; then HOST_PROTOC="${prefix}/bin/protoc"; return; fi
    info "[protobuf] host protoc"
    # Build with the host compiler, without the target's flags (Yocto exports them).
    run_logged "${LOG_DIR}/host-protobuf-configure.log" \
        env -u CFLAGS -u CXXFLAGS -u LDFLAGS -u CPPFLAGS -u PKG_CONFIG_SYSROOT_DIR -u PKG_CONFIG_PATH \
        CC="${HOST_CC:-cc}" CXX="${HOST_CXX:-c++}" \
        cmake -S "${TP_DIR}/protobuf" -B "${bdir}" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${prefix}" -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_EXAMPLES=OFF \
        -Dprotobuf_WITH_ZLIB=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF ${GENERATOR:+-G "${GENERATOR}"}
    run_logged "${LOG_DIR}/host-protobuf-build.log" \
        env -u CFLAGS -u CXXFLAGS -u LDFLAGS -u CPPFLAGS cmake --build "${bdir}" --parallel "${JOBS}" --target protoc
    mkdir -p "${prefix}/bin"
    cp "$(find "${bdir}" -maxdepth 2 -type f -name 'protoc*' -perm -u+x | head -n1)" "${prefix}/bin/protoc"
    HOST_PROTOC="${prefix}/bin/protoc"
}

if [[ -n "${TOOLCHAIN}" && -z "${HOST_PROTOC}" ]]; then
    if [[ "${SYSTEM_PROTOBUF}" == "ON" ]]; then
        HOST_PROTOC="$(command -v protoc || true)"
        [[ -n "${HOST_PROTOC}" ]] || die "--system-protobuf with --toolchain needs --protoc (the host protoc)"
    elif [[ "${SKIP_DEPS}" != "ON" || ! -x "${BUILD_DIR}/host-tools/bin/protoc" ]]; then
        build_host_protoc
    else
        HOST_PROTOC="${BUILD_DIR}/host-tools/bin/protoc"
    fi
fi
if [[ -n "${HOST_PROTOC}" ]]; then
    COMMON_ARGS+=("-DFORGE_PROTOC=${HOST_PROTOC}")
fi

if [[ "${SKIP_DEPS}" != "ON" ]]; then
    for dep in "${DEPS[@]}"; do
        if [[ "${SYSTEM_PROTOBUF}" == "ON" && "${dep}" == "protobuf" ]]; then continue; fi
        if [[ "${WITH_IIO}" == "OFF" && "${dep}" == "libiio" ]]; then continue; fi
        # shellcheck disable=SC2046
        build_project "${dep}" "${TP_DIR}/${dep}" $(dep_args "${dep}")
    done
fi

if [[ "${DEPS_ONLY}" != "ON" ]]; then
    build_project forgecpp "${SRC_DIR}" \
        "-DBUILD_SHARED_LIBS=${SHARED}" \
        "-DFORGECPP_BUILD_TESTS=${BUILD_TESTS}" \
        "-DFORGECPP_BUILD_EXAMPLES=${BUILD_EXAMPLES}" \
        "-DFORGECPP_BUILD_HUB=${WITH_HUB}" \
        "-DFORGECPP_WARNINGS=ON" \
        "-DFORGECPP_SANITIZE=${SANITIZE//,/;}"

    if [[ "${RUN_TESTS}" == "ON" ]]; then
        if [[ -n "${TOOLCHAIN}" ]]; then
            warn "cross build: skipping test execution (binaries are in ${BUILD_DIR}/forgecpp/tests)"
        else
            info "[forgecpp] test"
            run_logged "${LOG_DIR}/forgecpp-test.log" \
                ctest --test-dir "${BUILD_DIR}/forgecpp" --output-on-failure --parallel "${JOBS}"
        fi
    fi
fi

# Keep a combined manifest in the prefix so `install.sh --uninstall` works
# even after the build directory is gone (e.g. installs done through get.sh).
if [[ -z "${DESTDIR_ARG}" && "${DEPS_ONLY}" != "ON" && "${SKIP_DEPS}" != "ON" ]]; then
    MANIFEST_DIR="${PREFIX}/share/forgecpp"
    ${SUDO} mkdir -p "${MANIFEST_DIR}"
    for m in "${BUILD_DIR}"/*/install_manifest.txt; do cat "$m"; echo; done | grep -v '^$' |
        ${SUDO} tee "${MANIFEST_DIR}/install_manifest.txt" >/dev/null
fi

# Refresh the dynamic linker cache for native system-wide installs.
if [[ -z "${DESTDIR_ARG}" && -z "${TOOLCHAIN}" && "${SHARED}" == "ON" ]] && command -v ldconfig >/dev/null 2>&1; then
    if [[ "$(id -u)" -eq 0 || -n "${SUDO}" ]]; then
        ${SUDO} ldconfig 2>/dev/null || true
    fi
fi

if [[ "${PACKAGE}" == "ON" ]]; then
    if [[ -z "${ARCH}" ]]; then
        if [[ -n "${TOOLCHAIN}" ]]; then
            ARCH="$(sed -n 's/.*CMAKE_SYSTEM_PROCESSOR[ "]*\([A-Za-z0-9_]*\).*/\1/p' "${TOOLCHAIN}" | head -n1)"
        fi
        [[ -n "${ARCH}" ]] || ARCH="$(uname -m)"
    fi
    PKG="${BUILD_DIR}/forgecpp-${FORGECPP_VERSION}-${ARCH}.tar.gz"
    info "Creating ${PKG}"
    tar -C "${DESTDIR_ARG}" -czf "${PKG}" .
    echo
    info "Package ready: ${PKG}"
    echo "    install on a target with:  tar -xzf $(basename "${PKG}") -C /"
    exit 0
fi

if [[ "${DEPS_ONLY}" == "ON" ]]; then
    info "third-party libraries installed into ${DESTDIR_ARG}${PREFIX}"
    exit 0
fi

LIBDIR_SHOW="${LIBDIR:-lib}"
echo
info "forgecpp ${FORGECPP_VERSION} installed into ${DESTDIR_ARG}${PREFIX}"
cat <<EOF

  Use it from CMake:
      find_package(forgecpp REQUIRED)
      forge_add_app(mydaemon SOURCES main.cpp)    # or: target_link_libraries(x PRIVATE forgecpp::forgecpp)

  Create a new application skeleton:
      ${PREFIX}/bin/forgecpp-new mydaemon

EOF
if [[ "${WITH_HUB}" == "ON" ]]; then
    cat <<EOF
  Device dashboard (optional; applications register with it by themselves):
      sudo ${PREFIX}/bin/forge-hub -c ${PREFIX}/etc/forge-hub/forge-hub.toml   # http://<device>/
      sudo systemctl enable --now forge-hub                                      # systemd unit installed

EOF
fi
if [[ "${PREFIX}" != "/usr" && "${PREFIX}" != "/usr/local" ]]; then
    cat <<EOF
  Non-standard prefix: tell CMake where to look:
      export CMAKE_PREFIX_PATH=${PREFIX}:\$CMAKE_PREFIX_PATH
      export PKG_CONFIG_PATH=${PREFIX}/${LIBDIR_SHOW}/pkgconfig:\$PKG_CONFIG_PATH

EOF
fi
