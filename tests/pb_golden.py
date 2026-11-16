#!/usr/bin/env python3
"""Random forge_test.all_types messages, as Google's protobuf for Python writes
them: one JSON line per message, {"json": ..., "hex": ...}. test_pb.cpp checks
that forgecpp reads and writes the same JSON and the same bytes.

    pb_golden.py <dir with forge_test/all_types_pb2.py> <output file>
"""
import json
import random
import struct
import sys

sys.path.insert(0, sys.argv[1])
from google.protobuf import json_format  # noqa: E402
from forge_test import all_types_pb2 as pb  # noqa: E402

rng = random.Random(7)
LETTERS = "abcdefghijklmnopqrstuvwxyz ÄÖÜçğış0123456789"


def text(max_bytes):
    while True:
        s = "".join(rng.choice(LETTERS) for _ in range(rng.randint(0, max_bytes)))
        if len(s.encode()) <= max_bytes:
            return s


def f32():
    return struct.unpack("f", struct.pack("f", rng.uniform(-1e6, 1e6)))[0]


def inner(m):
    m.a = rng.randint(-2**31, 2**31 - 1)
    m.s = text(8)


def message():
    m = pb.all_types()
    m.b = rng.random() < 0.5
    m.i32 = rng.randint(-2**31, 2**31 - 1)
    m.i64 = rng.randint(-2**63, 2**63 - 1)
    m.u32 = rng.randint(0, 2**32 - 1)
    m.u64 = rng.randint(0, 2**64 - 1)
    m.s32 = rng.randint(-2**31, 2**31 - 1)
    m.s64 = rng.randint(-2**63, 2**63 - 1)
    m.f32 = rng.randint(0, 2**32 - 1)
    m.f64 = rng.randint(0, 2**64 - 1)
    m.sf32 = rng.randint(-2**31, 2**31 - 1)
    m.sf64 = rng.randint(-2**63, 2**63 - 1)
    m.fl = f32()
    m.db = rng.choice([rng.uniform(-1e12, 1e12), 1e300, float("inf"), 0.5])
    m.text = text(16)
    m.data = bytes(rng.randint(0, 255) for _ in range(rng.randint(0, 8)))
    m.key = bytes(rng.randint(1, 255) for _ in range(4))
    m.colour = rng.choice([0, 1, 2, 7])
    if rng.random() < 0.7:
        inner(m.one)
    m.list.extend(rng.randint(-2**31, 2**31 - 1) for _ in range(rng.randint(0, 4)))
    for _ in range(rng.randint(0, 3)):
        inner(m.many.add())
    m.words.extend(text(5) for _ in range(rng.randint(0, 3)))
    choice = rng.randint(0, 3)
    if choice == 1:
        m.number = rng.randint(0, 2**32 - 1)
    elif choice == 2:
        m.name = text(7)
    elif choice == 3:
        inner(m.nested)
    if rng.random() < 0.5:
        m.maybe = rng.randint(-2**31, 2**31 - 1)
    return m


def to_dict(m):
    # Fields without presence are printed with their defaults too. protobuf
    # 5.26 renamed the option; Ubuntu 24.04 and Yocto scarthgap have 4.21.
    try:
        return json_format.MessageToDict(m, always_print_fields_with_no_presence=True,
                                         preserving_proto_field_name=True)
    except TypeError:
        return json_format.MessageToDict(m, including_default_value_fields=True, preserving_proto_field_name=True)


with open(sys.argv[2], "w") as out:
    for _ in range(500):
        m = message()
        j = to_dict(m)
        out.write(json.dumps({"json": j, "hex": m.SerializeToString().hex()}, ensure_ascii=False) + "\n")
