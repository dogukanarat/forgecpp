# The forgecpp source, shared by the recipes of this layer (forgecpp,
# forgecpp-tools, the example applications): one place for the URL and the
# commit.
#
# FORGECPP_SRCREV is the commit of the release this layer ships (set by
# tools/release.sh); a development build points it at a branch head with
#   FORGECPP_SRCREV = "${AUTOREV}"
# in local.conf (or builds the checkout with externalsrc, as kas/ does).
HOMEPAGE = "https://github.com/dogukanarat/forgecpp"
FORGECPP_PV = "0.4.0"
FORGECPP_SRCREV ?= "FORGECPP_RELEASE_COMMIT"
FORGECPP_BRANCH ?= "master"

# gitsm: the parts compiled into forgecpp are submodules (nanopb, linenoise,
# BS::thread_pool); the libraries it links come from their own recipes.
SRC_URI = "gitsm://github.com/dogukanarat/forgecpp.git;protocol=https;branch=${FORGECPP_BRANCH}"
SRCREV = "${FORGECPP_SRCREV}"
S = "${UNPACKDIR}/git"

FORGECPP_LICENSE_CHKSUM = "file://${UNPACKDIR}/git/LICENSE;md5=d651bd6255f4ea85c3b1b170b8dd19c6"
