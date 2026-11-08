#!/usr/bin/env python3
"""Check that each forgecpp module uses only modules of its own level or lower.

A module is a header include/forge/<name>.hpp with its source src/<name>.cpp
(and src/app/*.cpp for the module "application"). A module "uses" another one
when one of its files includes the header of that module. The levels are in
LEVELS below; docs/CODING_STANDARDS.md (F-011) explains them.

    tools/check-layers.py            # exit status 1 and a list of errors on a bad dependency
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# From the lowest level to the highest.
LEVELS = {
    0: ("core", ["status", "error", "json", "trace", "crash", "proto", "config", "config_schema", "channel",
                 "thread_pool", "buffer_pool", "rt", "systemd", "version"]),
    1: ("services", ["log", "log_rt", "metrics", "events", "scheduler", "health", "alarms", "audit", "auth",
                     "assets"]),
    2: ("link", ["c2", "zmq", "http", "hub"]),
    3: ("front ends", ["console", "telnet", "web"]),
    4: ("application", ["component", "control", "application"]),
}
# Not modules: the umbrella header and the test helpers may include anything.
IGNORED = {"forge", "testing"}

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]forge/([a-z0-9_]+)\.hpp[>"]', re.M)


def level_of():
    out = {}
    for level, (_, names) in LEVELS.items():
        for name in names:
            if name in out:
                sys.exit(f"check-layers: '{name}' is in two levels")
            out[name] = level
    return out


def files_of(name):
    files = [os.path.join(ROOT, "include", "forge", name + ".hpp"), os.path.join(ROOT, "src", name + ".cpp")]
    if name == "application":
        app = os.path.join(ROOT, "src", "app")
        files += [os.path.join(app, f) for f in sorted(os.listdir(app)) if f.endswith((".cpp", ".hpp"))]
    return [f for f in files if os.path.exists(f)]


def main():
    levels = level_of()
    headers = sorted(f[:-4] for f in os.listdir(os.path.join(ROOT, "include", "forge")) if f.endswith(".hpp"))
    errors = []
    for name in headers:
        if name not in IGNORED and name not in levels:
            errors.append(f"{name}: not in a level; add it to LEVELS in tools/check-layers.py")
    for name, level in sorted(levels.items(), key=lambda kv: (kv[1], kv[0])):
        for path in files_of(name):
            for used in sorted(set(INCLUDE.findall(open(path).read()))):
                if used == name or used in IGNORED:
                    continue
                if used not in levels:
                    errors.append(f"{os.path.relpath(path, ROOT)}: includes '{used}', which is not in a level")
                elif levels[used] > level:
                    errors.append(f"{os.path.relpath(path, ROOT)}: '{name}' ({LEVELS[level][0]}) uses "
                                  f"'{used}' ({LEVELS[levels[used]][0]}), a higher level")
    for e in errors:
        print("check-layers:", e)
    if not errors:
        print(f"check-layers: {len(levels)} modules, every dependency goes to the same or a lower level")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
