# forgecpp Coding Standards

forgecpp is the Linux side of zephyrus. zephyrus runs on the
microcontrollers, and forgecpp runs on the Linux device (Z7020) and on the
ground side. The two must have the same maturity, and they must be compatible
on the wire. Thus forgecpp follows the rules of zephyrus
(`zephyrus/docs/CODING_STANDARDS.md`, `NAMING_STANDARDS.md`). This file gives
each rule an `F-` id and tells which zephyrus rule (`Z-`) it follows:

- **same**: the zephyrus rule applies without a change.
- **C++**: the rule applies in a form for C++ on Linux. The reason is given.
- **plan**: the rule applies to new code now. Old code changes in a later step.

When a rule cannot be followed, write the reason in a comment near the code.

---

## F-001 Compatible on the wire with zephyrus (no Z rule; forgecpp only)

Each format that also exists in zephyrus has the same bytes in forgecpp: the
c2 message, the status of a command (`forge::Status`), the command numbers
and the nanopb messages.

- Write the forgecpp code from the zephyrus code and its tests. Copy the byte
  vectors of the zephyrus unit tests into the forgecpp tests.
- `tests/compat/zephyrus/` builds the zephyrus C code on the host and compares
  it with forgecpp on the same input. Run it before a change to a wire format:

  ```sh
  cmake -B build -DFORGE_ZEPHYRUS_DIR=/path/to/zephyrus
  cmake --build build --target forgecpp_zephyrus_compat_tests
  ./build/tests/forgecpp_zephyrus_compat_tests
  ```

- Error codes on the wire use the errno numbers of Zephyr (newlib and
  picolibc). `forge::Status` converts them to the numbers of the host.

## F-002 Naming (Z-008, NAMING_STANDARDS.md section 3: same)

forgecpp uses the C++ part of the zephyrus naming rules:

| Kind | Rule | Example |
|---|---|---|
| Namespace | lowercase, the module name, in `forge` | `forge::c2`, `forge::log` |
| Class, struct | `PascalCase` | `Header`, `Status` |
| Function, method | `snake_case` | `decode_message()` |
| Member variable | `snake_case_` | `value_` |
| `enum class` and its values | `PascalCase` | `Kind::Request` |
| Constant (`constexpr`) | `kPascalCase` | `kHeaderSize`, `flag::kReq` |
| Units | in the name | `timeout_ms`, `rate_hz` |

Use one short form for one word: `req` and `rsp`, `hdr`, `config`.

A nanopb struct that both ends share keeps its C name
(`radio_tmtc_status_request`). It is the same struct as in zephyrus.

**plan:** Older forgecpp code has values of `enum class` in lowercase
(`log::Level::info`) and constants without `k`. They change in one step,
before the 0.4 release, with an entry in CHANGELOG.md.

## F-003 Fixed-width integers (Z-009: same)

Use `std::uint8_t` … `std::int64_t` for a value that crosses a wire, a file
or a register, and for counters and flags. Use `std::size_t` for sizes,
`bool` for booleans and `char` for text.

## F-004 The binary command path does not allocate and does not throw (Z-005, Z-020: C++)

The c2 codec, the tmtc registry and the link transports operate with fixed
memory, in the same way as in zephyrus:

- No heap allocation for a request: the request and the reply are nanopb
  structs with bounds (`max_size`, `max_count`), on the stack or in a fixed
  buffer of a worker.
- No exceptions: a function returns `forge::Status`. Status 0 is success.
- Refuse a value that is too large. Do not cut it (`ENAMETOOLONG`,
  `EMSGSIZE`).

Other parts of forgecpp (the web panel, the configuration, the logs) can
allocate and can throw `forge::Error`. They run on Linux with the full C++
library.

## F-005 Function structure (Z-007: C++)

zephyrus requires one return at the end, in a `for (;;) { … break; }` block,
because C has no destructors: the single exit keeps the cleanup in one place.

In C++ the cleanup is in destructors (RAII: `std::lock_guard`,
`std::unique_ptr`, containers). Thus a forgecpp function can return early, and
the cleanup stays correct. Use these rules:

- Put the argument checks at the top of the function, as guard clauses.
- Do not acquire a resource without an RAII owner. Then an early return
  cannot leak it.
- Declare a variable where it gets its first value. Give each variable an
  initial value.

## F-006 Equality comparisons (Z-012: C++)

zephyrus puts the constant on the left (`0U == count`) to stop `=` in place of
`==`. forgecpp compiles with `-Wall -Wextra`, and the compiler finds an
assignment in a condition. Thus forgecpp writes the variable first:
`if (status.ok())`, `if (count == 0)`.

## F-007 Errors carry their origin (Z-013: same)

An error of a forgecpp module has the module id of that module
(`forge::module_id::kC2`). forgecpp ids start at `module_id::kForgeBase`
(0x0100). This is above the ids of zephyrus modules (below 128). Give an
application error `module_id::kApp`. Register the name of a new module with
`forge::name_module()`.

## F-008 Commands: fixed-size, paired, numbered in one place (Z-020: same)

- Each command has a `<command>_request` and a `<command>_reply`, also when one
  of them is empty.
- The messages are in `<module>/proto/<module>.proto.in`. The bounds and the
  base number come from the build (CMake for forgecpp, Kconfig for zephyrus).
- A command number is `base + offset` from the `msg_id` enum of the module.
- Each owner has a block of 256 numbers. The table of blocks is in zephyrus
  Z-020. forgecpp modules take blocks from that table too, so that one image of
  each side can use the commands of the other side.

## F-009 Thin front ends (Z-001: same)

A console command, a web route and a tmtc handler only convert the input,
call the API of the module and convert the result. All behaviour is in the
API of the module, and a test can call it directly.

## F-010 Tests (Z-004, Z-019: same; layout: plan)

- Each behaviour has a test that checks its result, not only that it ran.
- Name a test for the behaviour it checks: `"c2: a short header is refused"`.
- Use the names of zephyrus for the scope of a suite: `unit`, `functional`,
  `integration`, `smoke`.
- **plan:** mutation testing (Z-019) for the modules of the command path.

## F-011 Modules and layers (Z-003, Z-022: plan)

zephyrus keeps each module in `modules/<layer>/<name>/` with its headers,
sources, tests and proto files. forgecpp is one library now. New modules use
the zephyrus names (`c2`, `tmtc`) and keep their files together
(`include/forge/c2.hpp`, `src/c2.cpp`, `tests/test_c2.cpp`). The move to the
module folders of zephyrus is a later step.

## F-012 Text (Z-021: same)

Comments, documentation, log messages, test names and commit messages use
ASD-STE100 Simplified Technical English: short sentences, active voice, one
word for one meaning. New and changed text obeys this rule.
