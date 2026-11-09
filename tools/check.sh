#!/usr/bin/env bash
# Everything CI would check, on this machine.
#
#   tools/check.sh                 format, layers, build, tests, fuzz, API and message compatibility
#   tools/check.sh --quick         format, layers, build, tests (no fuzz, no compatibility records)
#   tools/check.sh --format        only the formatting
#   tools/check.sh --fix           format the C++ files in place (clang-format)
#   tools/check.sh --tidy          clang-tidy on the C++ files changed since origin/master
#   tools/check.sh --tidy-all      clang-tidy on every source file
#   tools/check.sh --asan          also build and test with AddressSanitizer + UBSan
#   tools/check.sh --tsan PREFIX   also build and test with ThreadSanitizer (PREFIX: libraries built
#                                  with install.sh --sanitize thread; without it TSan reports
#                                  races inside them that are not there)
#   tools/check.sh --install-hook  run the quick checks before every commit (tools/git-hooks)
#
# The libraries come from FORGECPP_DEPS (a prefix of install.sh --deps-only, or
# CMAKE_PREFIX_PATH); build directories go to build/check-*.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"
DEPS="${FORGECPP_DEPS:-${CMAKE_PREFIX_PATH:-}}"
JOBS="$(nproc 2>/dev/null || echo 4)"

do_format=1 do_layers=1 do_build=1 do_fuzz=1 do_compat=1
do_tidy="" do_asan=0 tsan_prefix="" fix=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --quick)        do_fuzz=0; do_compat=0 ;;
        --format)       do_layers=0; do_build=0; do_fuzz=0; do_compat=0 ;;
        --fix)          fix=1 ;;
        --tidy)         do_tidy=changed ;;
        --tidy-all)     do_tidy=all ;;
        --asan)         do_asan=1 ;;
        --tsan)         tsan_prefix="${2:?--tsan needs the prefix of TSan-built libraries}"; shift ;;
        --install-hook) git config core.hooksPath tools/git-hooks
                        git config blame.ignoreRevsFile .git-blame-ignore-revs
                        echo "pre-commit hook on (tools/git-hooks/pre-commit); git commit --no-verify skips it"
                        echo "git blame skips the formatting commits (.git-blame-ignore-revs)"
                        exit 0 ;;
        -h|--help)      sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)              echo "check: unknown option $1 (--help)" >&2; exit 2 ;;
    esac
    shift
done

failed=()
step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
fail() { failed+=("$1"); printf '\033[31mFAILED: %s\033[0m\n' "$1"; }
cpp_files() { git ls-files '*.cpp' '*.hpp' '*.h' | grep -v -e '^third_party/' -e '^tests/compat/zephyrus/private/' \
                  -e '^tests/compat/zephyrus/stubs/'; }

if [[ ${fix} == 1 ]]; then
    step "clang-format: fixing"
    cpp_files | xargs clang-format -i
    git diff --stat
    exit 0
fi

if [[ ${do_format} == 1 ]]; then
    step "format (clang-format)"
    if ! command -v clang-format >/dev/null; then
        echo "clang-format not found: skipped"
    elif ! cpp_files | xargs clang-format --dry-run --Werror 2>/tmp/forge-format.$$; then
        grep -o '^[^:]*' /tmp/forge-format.$$ | sort -u | sed 's/^/  /' || true
        fail "format: tools/check.sh --fix formats these files"
    fi
    rm -f /tmp/forge-format.$$
fi

if [[ ${do_layers} == 1 ]]; then
    step "module layers"
    python3 tools/check-layers.py || fail "layers"
fi

# configure_and_test <dir> <label> [cmake args...]
configure_and_test() {
    local dir="$1" label="$2"; shift 2
    step "build: ${label}"
    if ! cmake -S . -B "${dir}" -G Ninja -DCMAKE_PREFIX_PATH="${DEPS}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
            -DFORGECPP_BUILD_TESTS=ON -DFORGECPP_BUILD_EXAMPLES=ON "$@" >"${dir}.configure.log" 2>&1; then
        tail -20 "${dir}.configure.log"; fail "configure (${label})"; return 1
    fi
    if ! cmake --build "${dir}" -j "${JOBS}" >"${dir}.build.log" 2>&1; then
        grep -E 'error|FAILED' -A3 "${dir}.build.log" | head -40 || true  # only shown: pipefail must not stop us
        fail "build (${label})"; return 1
    fi
    step "tests: ${label}"
    local exclude=()
    [[ ${do_fuzz} == 0 ]] && exclude=(-LE fuzz)
    ctest --test-dir "${dir}" -j "${JOBS}" --timeout 900 --output-on-failure "${exclude[@]}" || fail "tests (${label})"
}

mkdir -p build
if [[ ${do_build} == 1 ]]; then
    configure_and_test build/check-release release -DCMAKE_BUILD_TYPE=Release
fi

if [[ ${do_compat} == 1 && -f build/check-release/libforgecpp.so ]]; then
    step "C++ API and configuration keys (api/forgecpp-<x.y>.json)"
    python3 tools/api-check.py --lib build/check-release/libforgecpp.so --app build/check-release/examples/hello ||
        fail "api-check"
    step "messages (proto/forge/API.json)"
    protoc="$(command -v protoc || true)"
    [[ -z "${protoc}" && -x "${DEPS%%;*}/bin/protoc" ]] && protoc="${DEPS%%;*}/bin/protoc"
    [[ -z "${protoc}" && -x "${DEPS%%;*}/share/forgecpp/protoc" ]] && protoc="${DEPS%%;*}/share/forgecpp/protoc"
    if [[ -n "${protoc}" ]]; then
        python3 tools/proto-compat.py --protoc "${protoc}" || fail "proto-compat"
    else
        echo "protoc not found: skipped"
    fi
fi

if [[ -n "${do_tidy}" ]]; then
    step "clang-tidy (${do_tidy})"
    db=build/check-release
    [[ -f ${db}/compile_commands.json ]] || { fail "clang-tidy needs a build (run without --format first)"; db=""; }
    if [[ -n "${db}" ]]; then
        if [[ ${do_tidy} == changed ]]; then
            base="$(git merge-base HEAD origin/master 2>/dev/null || git rev-list --max-parents=0 HEAD)"
            mapfile -t files < <(git diff --name-only "${base}" -- '*.cpp' | grep -E '^(src|hub/src|tools)/' || true)
        else
            mapfile -t files < <(git ls-files 'src/*.cpp' 'src/app/*.cpp' 'hub/src/*.cpp' 'tools/*.cpp')
        fi
        if [[ ${#files[@]} -eq 0 ]]; then
            echo "no changed source files"
        else
            out="$(clang-tidy -p "${db}" --quiet "${files[@]}" 2>/dev/null || true)"
            if grep -q 'warning:' <<<"${out}"; then
                grep -A3 'warning:' <<<"${out}"; fail "clang-tidy"
            else
                echo "${#files[@]} files, no findings"
            fi
        fi
    fi
fi

if [[ ${do_asan} == 1 ]]; then
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 FORGE_TEST_TIMEOUT_SCALE=4 \
        configure_and_test build/check-asan asan -DCMAKE_BUILD_TYPE=Debug '-DFORGECPP_SANITIZE=address;undefined'
fi

if [[ -n "${tsan_prefix}" ]]; then
    DEPS="${tsan_prefix}" TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 FORGE_TEST_TIMEOUT_SCALE=4 \
        configure_and_test build/check-tsan tsan -DCMAKE_BUILD_TYPE=Debug -DFORGECPP_SANITIZE=thread
fi

echo
if [[ ${#failed[@]} -gt 0 ]]; then
    printf '\033[31m%d failed:\033[0m %s\n' "${#failed[@]}" "${failed[*]}"
    exit 1
fi
printf '\033[32mall checks passed\033[0m\n'
