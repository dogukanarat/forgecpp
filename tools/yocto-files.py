#!/usr/bin/env python3
"""Check that the Yocto recipes package every file their builds install.

bitbake stops with an "installed-vs-shipped" QA error when a recipe's
do_install leaves a file that none of its packages (FILES) holds. That is
found only after an hour of building the image; this finds it in seconds,
from staged installs of the same CMake projects:

    DESTDIR=$st/quill cmake --install build/quill --prefix /usr
    ...
    tools/yocto-files.py --orig-prefix $HOME/forgecpp \\
        --stage quill=$st/quill --stage forgecpp=$st/forgecpp --examples $st/examples

--stage PN=DIR    a DESTDIR tree to check against the recipe PN of the layer
                  (recipes-*/*/PN_*.bb, or backfill/*/recipes-*/*/PN_*.bb)
--examples DIR    one DESTDIR tree per example application (DIR/<name>), each
                  built on its own against the installed forgecpp, as its
                  recipe does; those with a recipe in the layer are checked
--orig-prefix P   a prefix the builds were configured with (repeatable): files
                  staged under it (absolute install paths) are moved to /usr

The packaging rules are those of scarthgap's bitbake.conf and systemd.bbclass
(usrmerge, as INIT_MANAGER = "systemd" sets) plus the recipe's own PACKAGES,
FILES and SYSTEMD_SERVICE. A file in two stages is reported too: two packages
in one image cannot own the same path.

It does not run bitbake: options a recipe sets (EXTRA_OECMAKE) and the ones
the staged build used may differ, and do_install:append steps are not run.
"""

import argparse
import fnmatch
import glob
import os
import re
import sys

PYTHON = "python3.12"  # scarthgap's PYTHON_BASEVERSION

# bitbake.conf (scarthgap) with usrmerge
BASE = {
    "prefix": "/usr",
    "exec_prefix": "/usr",
    "base_prefix": "",
    "root_prefix": "/usr",
    "bindir": "/usr/bin",
    "sbindir": "/usr/sbin",
    "libdir": "/usr/lib",
    "libexecdir": "/usr/libexec",
    "includedir": "/usr/include",
    "datadir": "/usr/share",
    "docdir": "/usr/share/doc",
    "mandir": "/usr/share/man",
    "infodir": "/usr/share/info",
    "sysconfdir": "/etc",
    "localstatedir": "/var",
    "sharedstatedir": "/com",
    "base_bindir": "/usr/bin",
    "base_sbindir": "/usr/sbin",
    "base_libdir": "/usr/lib",
    "nonarch_base_libdir": "/usr/lib",
    "systemd_unitdir": "/usr/lib/systemd",
    "systemd_system_unitdir": "/usr/lib/systemd/system",
    "systemd_user_unitdir": "/usr/lib/systemd/user",
    "PYTHON_SITEPACKAGES_DIR": f"/usr/lib/{PYTHON}/site-packages",
    "SOLIBS": ".so.*",
    "SOLIBSDEV": ".so",
    "PACKAGE_BEFORE_PN": "",
    # kas/qemuarm64-poky.yml: systemd, ptest
    "DISTRO_FEATURES": "systemd usrmerge ptest",
    "BBFILE_COLLECTIONS": "core forgecpp",
}

DEFAULTS = """
PACKAGES = "${PN}-src ${PN}-dbg ${PN}-staticdev ${PN}-dev ${PN}-doc ${PN}-locale ${PACKAGE_BEFORE_PN} ${PN}"
FILES:${PN} = "${bindir}/* ${sbindir}/* ${libexecdir}/* ${libdir}/lib*${SOLIBS} \\
    ${sysconfdir} ${sharedstatedir} ${localstatedir} ${base_bindir}/* ${base_sbindir}/* \\
    ${base_libdir}/*${SOLIBS} ${base_prefix}/lib/udev ${prefix}/lib/udev ${base_libdir}/udev ${libdir}/udev \\
    ${datadir}/${BPN} ${libdir}/${BPN}/* ${datadir}/pixmaps ${datadir}/applications \\
    ${datadir}/idl ${datadir}/omf ${datadir}/sounds ${libdir}/bonobo/servers"
FILES:${PN}-doc = "${docdir} ${mandir} ${infodir} ${datadir}/gtk-doc ${datadir}/gnome/help"
FILES_SOLIBSDEV = "${base_libdir}/lib*${SOLIBSDEV} ${libdir}/lib*${SOLIBSDEV}"
FILES:${PN}-dev = "${includedir} ${FILES_SOLIBSDEV} ${libdir}/*.la ${libdir}/*.o ${libdir}/pkgconfig \\
    ${datadir}/pkgconfig ${datadir}/aclocal ${base_libdir}/*.o ${libdir}/${BPN}/*.la ${base_libdir}/*.la \\
    ${libdir}/cmake ${datadir}/cmake"
FILES:${PN}-staticdev = "${libdir}/*.a ${base_libdir}/*.a ${libdir}/${BPN}/*.a"
FILES:${PN}-dbg = "/usr/lib/debug /usr/lib/debug-static /usr/src/debug"
FILES:${PN}-locale = "${datadir}/locale"
SYSTEMD_PACKAGES = "${PN}"
"""


class Recipe:
    def __init__(self, path, layer):
        self.path = path
        self.layer = layer
        pn = os.path.basename(path).split("_")[0]
        self.vars = dict(BASE, PN=pn, BPN=pn)
        self.notes = []
        self._parse_text(DEFAULTS, "bitbake.conf")
        self._parse_file(path)

    # -- parsing ---------------------------------------------------------
    def _parse_file(self, path):
        with open(path, encoding="utf-8") as f:
            self._parse_text(f.read(), path)

    def _parse_text(self, text, where):
        text = re.sub(r"\\\n", " ", text)
        in_func = False
        for line in text.splitlines():
            s = line.strip()
            if in_func:
                if s == "}":
                    in_func = False
                continue
            if not s or s.startswith("#"):
                continue
            if re.match(r"^[\w:${}.-]+\s*\(\)\s*\{", s) or s.startswith("python ") or s.startswith("def "):
                in_func = not s.endswith("}")
                continue
            m = re.match(r"^(require|include)\s+(\S+)", s)
            if m:
                inc = self.expand(m.group(2))
                for base in (self.layer, os.path.dirname(where)):
                    p = os.path.join(base, inc)
                    if os.path.isfile(p):
                        self._parse_file(p)
                        break
                else:
                    if m.group(1) == "require":
                        self.notes.append(f"{where}: cannot find {inc}")
                continue
            m = re.match(r'^([\w:${}.+-]+?)\s*(\?\?=|\?=|:=|\+=|=\+|\.=|=\.|=)\s*"(.*)"\s*$', s)
            if not m:
                continue
            name, op, value = self.expand(m.group(1)), m.group(2), m.group(3)
            if name.endswith(":append") or name.endswith(":prepend"):
                base, kind = name.rsplit(":", 1)
                old = self.vars.get(base, "")
                self.vars[base] = old + value if kind == "append" else value + old
                continue
            old = self.vars.get(name)
            if op in ("=", ":="):
                self.vars[name] = value
            elif op in ("?=", "??="):
                if old is None:
                    self.vars[name] = value
            elif op == "+=":
                self.vars[name] = (old + " " + value) if old else value
            elif op == "=+":
                self.vars[name] = (value + " " + old) if old else value
            elif op == ".=":
                self.vars[name] = (old or "") + value
            elif op == "=.":
                self.vars[name] = value + (old or "")

    # -- expansion -------------------------------------------------------
    def expand(self, s, depth=0):
        if depth > 20:
            return s
        def python(m):
            expr = m.group(1)
            c = re.match(r"""bb\.utils\.contains\(\s*['"](\w+)['"]\s*,\s*['"]([^'"]*)['"]\s*,"""
                         r"""\s*['"]([^'"]*)['"]\s*,\s*['"]([^'"]*)['"]\s*,\s*d\s*\)""", expr)
            if c:
                have = self.expand(self.vars.get(c.group(1), ""), depth + 1).split()
                want = c.group(2).split()
                return c.group(3) if all(w in have for w in want) else c.group(4)
            self.notes.append(f"not evaluated: ${{@{expr}}}")
            return ""
        out = re.sub(r"\$\{@(.*?)\}(?=\s|$|\")", python, s)
        out = re.sub(r"\$\{(\w+)\}", lambda m: self.vars.get(m.group(1), m.group(0)), out)
        return out if out == s else self.expand(out, depth + 1)

    def get(self, name):
        return self.expand(self.vars.get(self.expand(name), ""))

    # -- packages --------------------------------------------------------
    def packages(self, staged):
        """[(package, [patterns])] in bitbake's order (the first that matches takes the file)."""
        pkgs = self.get("PACKAGES").split()
        files = {p: self.get(f"FILES:{p}").split() for p in pkgs}
        # systemd.bbclass: each unit of SYSTEMD_SERVICE (with its @ template and Also=)
        for pkg in self.get("SYSTEMD_PACKAGES").split():
            for unit in self.get(f"SYSTEMD_SERVICE:{pkg}").split():
                for path in self._units(unit, staged, set()):
                    files.setdefault(pkg, []).append(path)
        return [(p, files.get(p, [])) for p in pkgs]

    def _units(self, unit, staged, seen):
        if unit in seen:
            return []
        seen.add(unit)
        out = []
        for d in ("/etc/systemd/system", BASE["systemd_system_unitdir"], BASE["systemd_user_unitdir"]):
            path = f"{d}/{unit}"
            if path in staged:
                out.append(path)
                if unit.endswith(".service"):
                    out += self._units(unit[: -len(".service")] + "@.service", staged, seen)
                for line in staged[path].get("also", []):
                    for also in line.split(","):
                        out += self._units(also.strip(), staged, seen)
                break
        return out


def matches(path, pattern):
    """A FILES entry: a glob for a file, or for a directory whose contents it takes."""
    if not pattern.startswith("/"):
        return False
    pp = pattern.rstrip("/").split("/")
    fp = path.split("/")
    if len(fp) < len(pp):
        return False
    return all(fnmatch.fnmatchcase(a, b) for a, b in zip(fp, pp))


def normalize(path, orig_prefixes):
    for p in orig_prefixes:
        p = p.rstrip("/")
        if p and p != "/usr" and (path == p or path.startswith(p + "/")):
            path = "/usr" + path[len(p):]
            break
    for a, b in (("/usr/etc/", "/etc/"), ("/usr/var/", "/var/"), ("/lib/systemd/", "/usr/lib/systemd/")):
        if path.startswith(a):
            path = b + path[len(a):]
    path = re.sub(r"^/usr/lib/python3(\.\d+)?/(site|dist)-packages/", f"/usr/lib/{PYTHON}/site-packages/", path)
    return path


def staged_files(root, orig_prefix):
    """{installed path: {"src": staged path, "also": [Also= values]}}"""
    out = {}
    for dirpath, dirnames, filenames in os.walk(root):
        names = filenames + [d for d in dirnames if os.path.islink(os.path.join(dirpath, d))]
        for name in names:
            full = os.path.join(dirpath, name)
            path = normalize("/" + os.path.relpath(full, root), orig_prefix)
            info = {"src": full}
            if path.endswith((".service", ".socket")) and os.path.isfile(full):
                with open(full, encoding="utf-8", errors="replace") as f:
                    info["also"] = [l.split("=", 1)[1].strip() for l in f if l.startswith("Also=")]
            out[path] = info
    return out


def find_recipe(layer, pn):
    hits = sorted(glob.glob(os.path.join(layer, "recipes-*", "*", f"{pn}_*.bb")) +
                  glob.glob(os.path.join(layer, "backfill", "*", "recipes-*", "*", f"{pn}_*.bb")))
    return hits[0] if hits else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--layer", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "yocto"))
    ap.add_argument("--stage", action="append", default=[], metavar="PN=DIR")
    ap.add_argument("--examples", metavar="DIR")
    ap.add_argument("--orig-prefix", action="append", default=[])
    ap.add_argument("-v", "--verbose", action="store_true", help="list every file with its package")
    args = ap.parse_args()

    stages = {}
    for spec in args.stage:
        pn, _, d = spec.partition("=")
        stages[pn] = staged_files(d, args.orig_prefix)
    if args.examples:
        for name in sorted(os.listdir(args.examples)):
            if find_recipe(args.layer, name):
                stages[name] = staged_files(os.path.join(args.examples, name), args.orig_prefix)

    failed = False
    owner = {}
    for pn, files in sorted(stages.items()):
        recipe_path = find_recipe(args.layer, pn)
        if not recipe_path:
            print(f"{pn}: no recipe in {args.layer}")
            failed = True
            continue
        recipe = Recipe(recipe_path, args.layer)
        pkgs = recipe.packages(files)
        lost = []
        for path in sorted(files):
            pkg = next((p for p, pats in pkgs if any(matches(path, x) for x in pats)), None)
            if pkg is None:
                lost.append(path)
            elif args.verbose:
                print(f"  {pkg:24} {path}")
            owner.setdefault(path, []).append(pn)
        rel = os.path.relpath(recipe_path, os.path.join(args.layer, ".."))
        if lost:
            failed = True
            print(f"{pn} ({rel}): {len(lost)} of {len(files)} files in no package (installed-vs-shipped):")
            for path in lost:
                print(f"    {path}")
        else:
            print(f"{pn}: {len(files)} files, all packaged")
        for note in dict.fromkeys(recipe.notes):
            print(f"    note: {note}")
    for path, pns in sorted(owner.items()):
        if len(pns) > 1:
            failed = True
            print(f"{path}: installed by {', '.join(pns)} (two packages cannot own one file)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
