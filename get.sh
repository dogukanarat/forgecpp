#!/usr/bin/env bash
# One-line installer for forgecpp.
#
#   curl -fsSL https://raw.githubusercontent.com/dogukanarat/forgecpp/master/get.sh | bash
#   curl -fsSL https://raw.githubusercontent.com/dogukanarat/forgecpp/master/get.sh | bash -s -- --prefix /opt/forgecpp --ref v0.3.0
#
# Options handled here (everything else is passed to install.sh):
#   --ref REF          branch or tag to install (default: the repository default branch)
#   --repo URL         git URL (default: https://github.com/dogukanarat/forgecpp.git)
#   --keep-source DIR  keep the checkout and build dir in DIR (default: temporary, removed)
#
# Environment overrides: FORGECPP_REPO, FORGECPP_REF.

set -euo pipefail

REPO="${FORGECPP_REPO:-https://github.com/dogukanarat/forgecpp.git}"
REF="${FORGECPP_REF:-}"
KEEP=""
PASS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ref)          REF="${2:?--ref needs a value}"; shift 2 ;;
        --repo)         REPO="${2:?--repo needs a value}"; shift 2 ;;
        --keep-source)  KEEP="${2:?--keep-source needs a value}"; shift 2 ;;
        *)              PASS+=("$1"); shift ;;
    esac
done

for tool in git cmake; do
    command -v "$tool" >/dev/null 2>&1 || { echo "forgecpp: '$tool' is required" >&2; exit 1; }
done

if [[ -n "${KEEP}" ]]; then
    WORK="${KEEP}"
    mkdir -p "${WORK}"
else
    WORK="$(mktemp -d "${TMPDIR:-/tmp}/forgecpp.XXXXXX")"
    trap 'rm -rf "${WORK}"' EXIT
fi

SRC="${WORK}/forgecpp"
CLONE_ARGS=(--depth 1 --recurse-submodules --shallow-submodules)
[[ -n "${REF}" ]] && CLONE_ARGS+=(--branch "${REF}")

if [[ -d "${SRC}/.git" ]]; then
    echo "==> Updating ${SRC}"
    git -C "${SRC}" fetch --depth 1 origin "${REF:-HEAD}"
    git -C "${SRC}" checkout -q FETCH_HEAD
    git -C "${SRC}" submodule update --init --recursive --depth 1
else
    echo "==> Cloning ${REPO}${REF:+ (${REF})}"
    git clone "${CLONE_ARGS[@]}" "${REPO}" "${SRC}"
fi

"${SRC}/install.sh" --build-dir "${WORK}/build" "${PASS[@]+"${PASS[@]}"}"
