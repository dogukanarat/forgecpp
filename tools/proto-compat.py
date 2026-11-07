#!/usr/bin/env python3
"""Check that proto/forge/v1 only changes compatibly with the released API.

proto/forge/v1/API.json records every message, field and enum value of the
last release (number, name, type, label). A change is compatible when
nothing recorded there changes: no field or enum value is renumbered,
retyped, renamed (the JSON mapping uses names) or removed without reserving
its number. Adding messages, fields and values is fine.

    tools/proto-compat.py --protoc build/protoc            # check (CI)
    tools/proto-compat.py --protoc build/protoc --update   # at a release: record the API

Needs protoc and the protobuf Python package.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

from google.protobuf import descriptor_pb2

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROTO_ROOT = os.path.join(ROOT, "proto")
PACKAGE_DIR = os.path.join(PROTO_ROOT, "forge", "v1")
SNAPSHOT = os.path.join(PACKAGE_DIR, "API.json")

FT = descriptor_pb2.FieldDescriptorProto


def describe(protoc):
    files = sorted(f"forge/v1/{f}" for f in os.listdir(PACKAGE_DIR) if f.endswith(".proto"))
    with tempfile.NamedTemporaryFile(suffix=".pb") as out:
        subprocess.run([protoc, f"-I{PROTO_ROOT}", f"--descriptor_set_out={out.name}", *files], check=True)
        fds = descriptor_pb2.FileDescriptorSet()
        fds.ParseFromString(open(out.name, "rb").read())
    api = {"messages": {}, "enums": {}}

    def field_type(f):
        if f.type in (FT.TYPE_MESSAGE, FT.TYPE_ENUM):
            return f.type_name.lstrip(".")
        return FT.Type.Name(f.type)[5:].lower()

    def walk_enum(e, prefix):
        api["enums"][prefix + e.name] = {
            "values": {str(v.number): v.name for v in e.value},
            "reserved": [[r.start, r.end] for r in e.reserved_range],  # inclusive end
            "reserved_names": list(e.reserved_name),
        }

    def walk_message(m, prefix):
        name = prefix + m.name
        api["messages"][name] = {
            "fields": {
                str(f.number): {"name": f.name, "type": field_type(f), "label": FT.Label.Name(f.label)[6:].lower(),
                                **({"oneof": m.oneof_decl[f.oneof_index].name} if f.HasField("oneof_index") else {})}
                for f in m.field
            },
            "reserved": [[r.start, r.end - 1] for r in m.reserved_range],  # exclusive end in descriptors
            "reserved_names": list(m.reserved_name),
        }
        for nested in m.nested_type:
            if not nested.options.map_entry:
                walk_message(nested, name + ".")
        for e in m.enum_type:
            walk_enum(e, name + ".")

    for f in fds.file:
        prefix = f.package + "."
        for m in f.message_type:
            walk_message(m, prefix)
        for e in f.enum_type:
            walk_enum(e, prefix)
    return api


def reserved(number, ranges):
    return any(lo <= number <= hi for lo, hi in ranges)


def check(old, new):
    problems = []
    for name, m in old["messages"].items():
        cur = new["messages"].get(name)
        if cur is None:
            problems.append(f"message {name} was removed")
            continue
        names_now = {f["name"]: n for n, f in cur["fields"].items()}
        for number, f in m["fields"].items():
            now = cur["fields"].get(number)
            where = f"{name}.{f['name']} = {number}"
            if now is None:
                if not reserved(int(number), cur["reserved"]):
                    problems.append(f"{where}: removed without `reserved {number};`")
                if f["name"] in names_now:
                    problems.append(f"{where}: its name now has number {names_now[f['name']]}")
                continue
            for key in ("name", "type", "label"):
                if now[key] != f[key]:
                    problems.append(f"{where}: {key} changed from {f[key]} to {now[key]}")
            if now.get("oneof") != f.get("oneof"):
                problems.append(f"{where}: moved into or out of a oneof")
    for name, e in old["enums"].items():
        cur = new["enums"].get(name)
        if cur is None:
            problems.append(f"enum {name} was removed")
            continue
        for number, value in e["values"].items():
            now = cur["values"].get(number)
            if now is None and not reserved(int(number), cur["reserved"]):
                problems.append(f"{name}.{value} = {number}: removed without reserving the number")
            elif now is not None and now != value:
                problems.append(f"{name} {number}: renamed from {value} to {now}")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--protoc", default="protoc")
    ap.add_argument("--update", action="store_true", help="record the current API (at a release)")
    args = ap.parse_args()
    api = describe(args.protoc)
    if args.update:
        with open(SNAPSHOT, "w") as f:
            json.dump(api, f, indent=1, sort_keys=True)
            f.write("\n")
        print(f"recorded {len(api['messages'])} messages, {len(api['enums'])} enums in {os.path.relpath(SNAPSHOT, ROOT)}")
        return 0
    with open(SNAPSHOT) as f:
        old = json.load(f)
    problems = check(old, api)
    for p in problems:
        print(f"incompatible: {p}", file=sys.stderr)
    if problems:
        print("forge.v1 must stay compatible with the released API (proto/forge/v1/API.json): "
              "add new fields instead, reserve removed numbers", file=sys.stderr)
        return 1
    print(f"forge.v1 is compatible with the released API ({len(old['messages'])} messages, {len(old['enums'])} enums)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
