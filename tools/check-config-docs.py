#!/usr/bin/env python3
"""Check that docs/application.md names every configuration key the framework declares.

    tools/check-config-docs.py build/check-release/examples/hello

Runs the application with --help-config (the keys the framework declares, with
their defaults) and looks for each key in the TOML block of the documentation:
a `[section]` header followed by `key = `. Deprecated keys (aliases) are not
required. Exit code 1 lists the keys the documentation leaves out.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(ROOT, "docs", "application.md")


def declared(app):
    out = subprocess.run([app, "--help-config"], check=True, capture_output=True, text=True).stdout
    keys, section = set(), ""
    for line in out.splitlines():
        if line.startswith("# Deprecated keys"):
            break
        m = re.match(r"^\[([a-z0-9_.]+)\]", line)
        if m:
            section = m.group(1)
            continue
        m = re.match(r"^#?\s*([a-z0-9_]+)\s*=", line)
        if m and section:
            keys.add(f"{section}.{m.group(1)}")
    return keys


def documented():
    keys, section = set(), ""
    for line in open(DOC):
        m = re.match(r"^\[([a-z0-9_.<>-]+)\]", line)
        if m:
            section = m.group(1)
            continue
        m = re.match(r"^#?\s*([a-z0-9_]+)\s*=", line)
        if m and section:
            keys.add(f"{section}.{m.group(1)}")
    return keys


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    # [threads.<name>] is per thread; [app] is the example application's own section.
    missing = sorted(k for k in declared(sys.argv[1]) if k not in documented()
                     and not k.startswith("threads.") and not k.startswith("app."))
    for k in missing:
        print(f"docs/application.md: key not documented: {k}", file=sys.stderr)
    if missing:
        return 1
    print("docs/application.md names every declared key")
    return 0


if __name__ == "__main__":
    sys.exit(main())
