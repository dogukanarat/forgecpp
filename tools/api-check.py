#!/usr/bin/env python3
"""Check that forgecpp's API stays compatible within a minor release.

api/forgecpp-<major>.<minor>.json records, for a release series:
  symbols      every function libforgecpp.so exports in namespace forge
               (forge::detail and the generated forge::v1 excepted): an
               application linked against libforgecpp.so.<major>.<minor>
               may use any of them, so none may disappear;
  config_keys  every configuration key the framework declares (keys in
               users' configuration files must keep working).

Within a series, adding is fine and removing fails. A new minor version
(new SOVERSION) starts a new record (--update); until 1.0 it may break, and
CHANGELOG.md says how. forge.v1 has its own check (tools/proto-compat.py).

    tools/api-check.py --lib build/libforgecpp.so --app build/examples/hello            # CI
    tools/api-check.py --lib build/libforgecpp.so --app build/examples/hello --update   # at a release
"""
import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def version():
    text = open(os.path.join(ROOT, "CMakeLists.txt")).read()
    m = re.search(r"project\(forgecpp\s+VERSION\s+(\d+)\.(\d+)\.(\d+)", text)
    return f"{m.group(1)}.{m.group(2)}"


def symbols(lib):
    out = subprocess.run(["nm", "-D", "--defined-only", lib], check=True, capture_output=True, text=True).stdout
    mangled = [line.split()[2] for line in out.splitlines() if len(line.split()) == 3 and line.split()[1] == "T"]
    names = subprocess.run(["c++filt"], input="\n".join(mangled), check=True, capture_output=True,
                           text=True).stdout.splitlines()
    keep = sorted({n for n in names
                   if n.startswith("forge::") and not n.startswith(("forge::detail::", "forge::v1::"))})
    return keep


def config_keys(app):
    out = subprocess.run([app, "--help-config"], check=True, capture_output=True, text=True).stdout
    keys, section = set(), ""
    for line in out.splitlines():
        m = re.match(r"^\[([^\]]+)\]", line)
        if m:
            section = m.group(1)
            continue
        m = re.match(r"^([A-Za-z0-9_]+) = ", line)
        if m and section:
            keys.add(f"{section}.{m.group(1)}")
    return sorted(keys)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lib", required=True, help="libforgecpp.so (shared build)")
    ap.add_argument("--app", required=True, help="an application with only the framework's keys (examples/hello)")
    ap.add_argument("--update", action="store_true", help="record the API of this release series")
    args = ap.parse_args()
    series = version()
    record = os.path.join(ROOT, "api", f"forgecpp-{series}.json")
    current = {"series": series, "symbols": symbols(args.lib), "config_keys": config_keys(args.app)}
    if args.update:
        os.makedirs(os.path.dirname(record), exist_ok=True)
        with open(record, "w") as f:
            json.dump(current, f, indent=1)
            f.write("\n")
        print(f"recorded {len(current['symbols'])} symbols, {len(current['config_keys'])} config keys in "
              f"{os.path.relpath(record, ROOT)}")
        return 0
    if not os.path.exists(record):
        print(f"no API record for {series} yet (new minor version): run with --update at the release")
        return 0
    old = json.load(open(record))
    problems = []
    now_symbols, now_keys = set(current["symbols"]), set(current["config_keys"])
    problems += [f"symbol removed: {s}" for s in old["symbols"] if s not in now_symbols]
    problems += [f"configuration key removed: {k}" for k in old["config_keys"] if k not in now_keys]
    for p in problems:
        print(f"incompatible: {p}", file=sys.stderr)
    if problems:
        print(f"forgecpp {series}.x must stay compatible with api/forgecpp-{series}.json: keep the old function "
              "(deprecate it), or make the change in the next minor version", file=sys.stderr)
        return 1
    added = len(now_symbols - set(old["symbols"])), len(now_keys - set(old["config_keys"]))
    print(f"forgecpp {series}: API compatible ({len(old['symbols'])} symbols, {len(old['config_keys'])} keys kept; "
          f"{added[0]} symbols and {added[1]} keys added)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
