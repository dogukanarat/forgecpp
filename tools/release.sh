#!/usr/bin/env bash
# Make a release commit and tag on this machine; pushing stays a separate step.
#
#   tools/release.sh 0.4.0            # checks, records the APIs, dates the CHANGELOG, commits, tags v0.4.0
#   tools/release.sh 0.4.0 --dry-run  # only the checks: says what it would do
#
# It refuses when: the tree has changes, the branch is not master (or main),
# project(VERSION) in CMakeLists.txt is not <version>, the CHANGELOG has no
# "## Unreleased (<version>)" section, the tag exists, or tools/check.sh fails.
# The libraries come from FORGECPP_DEPS, as for tools/check.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

VERSION="${1:-}"
DRY=0
[[ "${2:-}" == "--dry-run" ]] && DRY=1
if [[ ! "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
TAG="v${VERSION}"
fail() { echo "release: $*" >&2; exit 1; }

[[ -z "$(git status --porcelain)" ]] || fail "the tree has changes; commit or stash them first"
branch="$(git rev-parse --abbrev-ref HEAD)"
[[ "${branch}" == master || "${branch}" == main ]] || fail "on branch ${branch}: release from master (or main)"
git rev-parse -q --verify "refs/tags/${TAG}" >/dev/null && fail "tag ${TAG} exists"
grep -Eq "^    VERSION ${VERSION//./\\.}$" CMakeLists.txt || fail "CMakeLists.txt: project(VERSION) is not ${VERSION}"
grep -q "^## Unreleased (${VERSION})$" CHANGELOG.md || fail "CHANGELOG.md: no '## Unreleased (${VERSION})' section"

echo "== checks (tools/check.sh)"
tools/check.sh || fail "tools/check.sh failed"

if [[ ${DRY} == 1 ]]; then
    echo "dry run: would record the APIs, date the CHANGELOG, commit 'Release ${VERSION}' and tag ${TAG}"
    exit 0
fi

echo "== the API of this release"
build=build/check-release
python3 tools/api-check.py --lib "${build}/libforgecpp.so" --app "${build}/examples/hello" --update
protoc="$(command -v protoc || true)"
DEPS="${FORGECPP_DEPS:-${CMAKE_PREFIX_PATH:-}}"
[[ -z "${protoc}" && -x "${DEPS%%;*}/bin/protoc" ]] && protoc="${DEPS%%;*}/bin/protoc"
[[ -n "${protoc}" ]] || fail "no protoc to record proto/forge/API.json (set FORGECPP_DEPS)"
python3 tools/proto-compat.py --protoc "${protoc}" --update

echo "== CHANGELOG"
sed -i "s/^## Unreleased (${VERSION})$/## ${VERSION} ($(date -u +%Y-%m-%d))/" CHANGELOG.md

git add -A
git commit -q -m "Release ${VERSION}"
git tag -a "${TAG}" -m "forgecpp ${VERSION}"
release_commit="$(git rev-parse HEAD)"

# The Yocto layer ships the release: its recipes point at the tagged commit.
# This is a second commit (a commit cannot name its own hash); the tag stays
# on the release commit, the layer in master is what a BSP checks out.
echo "== the Yocto layer points at ${TAG}"
sed -i -e "s/^FORGECPP_PV = .*/FORGECPP_PV = \"${VERSION}\"/" \
       -e "s/^FORGECPP_SRCREV ?= .*/FORGECPP_SRCREV ?= \"${release_commit}\"/" \
       yocto/recipes-support/forgecpp/forgecpp-src.inc
git add yocto/recipes-support/forgecpp/forgecpp-src.inc
git commit -q -m "Yocto: the layer builds ${TAG}"
echo
echo "Release ${VERSION} is commit ${release_commit:0:7}, tag ${TAG}; the layer's commit follows it. To publish:"
echo "  git push origin ${branch} ${TAG}"
