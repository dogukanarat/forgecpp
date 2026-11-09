# The forgecpp source, shared by the recipes of this layer (forgecpp,
# forgecpp-tools, the example applications): one place for the URL and the
# commit.
#
# FORGECPP_SRCREV: tools/release.sh writes the commit of the release here, so
# a released layer builds exactly that commit. Between releases it follows the
# branch head (${AUTOREV}); a development build can also build a checkout
# with externalsrc, as kas/ does.
HOMEPAGE = "https://github.com/dogukanarat/forgecpp"
FORGECPP_PV = "0.4.0"
FORGECPP_SRCREV ?= "${AUTOREV}"
FORGECPP_BRANCH ?= "master"

# gitsm: the parts compiled into forgecpp are submodules (nanopb, linenoise,
# BS::thread_pool); the libraries it links come from their own recipes.
SRC_URI = "gitsm://github.com/dogukanarat/forgecpp.git;protocol=https;branch=${FORGECPP_BRANCH}"
SRCREV = "${FORGECPP_SRCREV}"
# The repository root, for every recipe (an application builds a
# subdirectory of it: OECMAKE_SOURCEPATH in forgecpp-app.inc).
S = "${FORGECPP_UNPACKDIR}/git"

FORGECPP_LICENSE_CHKSUM = "file://LICENSE;md5=d651bd6255f4ea85c3b1b170b8dd19c6"
