# forgecpp Coding Standards

forgecpp is the Linux side of zephyrus. zephyrus runs on the
microcontrollers, and forgecpp runs on the Linux device (Z7020) and on the
ground side. The two must have the same maturity, and they must be compatible
on the wire. forgecpp uses the principles of zephyrus
(`zephyrus/docs/CODING_STANDARDS.md`), in a form for C++ on Linux. Its naming
is its own (F-002).

Each rule has an `F-` id. Where a rule comes from a zephyrus rule, the `Z-` id
is given. A rule marked **plan** applies to new code now; old code changes in a
later step.

When a rule cannot be followed, write the reason in a comment near the code.

---

## F-001 Compatible on the wire with zephyrus

Each format that also exists in zephyrus has the same bytes in forgecpp: the
c2 message, the status of a command (`forge::Status`) and the nanopb messages.

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

## F-002 Naming

forgecpp follows the style of the C++ standard library for values and
functions, and uses `PascalCase` for its own types:

| Kind | Rule | Example |
|---|---|---|
| Namespace | lowercase, the module name, inside `forge` | `forge::c2`, `forge::log` |
| Class, struct, alias of a type, concept | `PascalCase` | `Header`, `Status`, `ThreadPool` |
| Function, method | `snake_case` | `decode_message()` |
| Variable, parameter | `snake_case` | `payload_len` |
| Private member variable | `snake_case_` | `value_` |
| Public data member of a plain struct | `snake_case`, no `_` | `Header::cmd_id` |
| `enum class` values | `snake_case` | `Kind::request`, `log::Level::warn` |
| Constants (`constexpr`, `const` at namespace scope) | `snake_case` | `c2::header_size`, `flag::req` |
| Template parameter | `PascalCase` | `template <class Message>` |
| Macro | `FORGE_UPPER_CASE` | `FORGE_INFO`, `FORGE_RT_WARN` |
| File | the module name, `snake_case` | `include/forge/c2.hpp`, `src/c2.cpp` |

- Units go in the name: `timeout_ms`, `rate_hz`, `size_bytes`.
- A boolean reads as a statement: `is_open`, `has_trace`, `ok()`.
- Use one short form for one word: `req` and `rsp`, `hdr`, `config`.
- Do not repeat the namespace in a name: `c2::Header`, not `c2::C2Header`.
- A nanopb struct keeps the C name that nanopb gives it
  (`radio_tmtc_status_request`). It is the same struct on both ends of a link.

## F-003 Fixed-width integers (Z-009)

Use `std::uint8_t` … `std::int64_t` for a value that crosses a wire, a file
or a register, and for counters and flags. Use `std::size_t` for sizes,
`bool` for booleans and `char` for text.

## F-004 The binary command path does not allocate and does not throw (Z-005, Z-020)

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

## F-005 Function structure (Z-007, in a form for C++)

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

## F-006 Equality comparisons (Z-012, in a form for C++)

zephyrus puts the constant on the left (`0U == count`) to stop `=` in place of
`==`. forgecpp compiles with `-Wall -Wextra`, and the compiler finds an
assignment in a condition. Thus forgecpp writes the variable first:
`if (status.ok())`, `if (count == 0)`.

## F-007 Errors carry their origin (Z-013)

An error of a forgecpp module has the module id of that module
(`forge::module_id::c2`). forgecpp ids start at `module_id::forge_base`
(0x0100). This is above the ids of zephyrus modules (below 128). Give an
application error `module_id::app`. Register the name of a new module with
`forge::name_module()`.

## F-008 Commands: fixed-size, paired, numbered once (Z-020)

- Each command has a `<command>_request` and a `<command>_reply`, also when one
  of them is empty.
- The messages are in `proto/<module>.proto.in`. The bounds and the base number
  come from CMake.
- A command number is `base + offset` from the `msg_id` enum of the module.
- A command number is unique in one program. forgecpp keeps its own table of
  bases (below); it does not share the table of zephyrus. The registry refuses
  a second command with the same number when it is registered.
- To send a command to a zephyrus device, use the `.proto` that the zephyrus
  build makes (`build/generated_proto/`): it has the numbers of that device.

| Block | Owner |
|---|---|
| `0x0000` | not a command: the zero value of each `msg_id` |
| `0x0001`–`0x00FF` | forgecpp itself (ping, information about the program) |
| `0x0100`–`0x7FFF` | the modules of the application, 256 numbers for each |
| `0xFF00`–`0xFFFF` | test suites |

## F-009 Thin front ends (Z-001)

A console command, a web route and a tmtc handler only convert the input,
call the API of the module and convert the result. All behaviour is in the
API of the module, and a test can call it directly.

## F-010 Tests (Z-004, Z-019)

- Each behaviour has a test that checks its result, not only that it ran.
- Name a test for the behaviour it checks: `"c2: a short header is refused"`.
- Use the names of zephyrus for the scope of a suite: `unit`, `functional`,
  `integration`, `smoke`.
- **plan:** mutation testing (Z-019) for the modules of the command path.

## F-011 Modules and levels (Z-003, Z-022)

forgecpp is one library. Each module is one header in `include/forge/`, one
source in `src/` and its tests. A module uses only modules of its own level or
of a lower level. `tools/check-layers.py` checks the includes; it runs as the
test `forgecpp_layers`.

| Level | Name | Modules |
|---|---|---|
| 0 | core | status, error, base64, json, pb, trace, crash, proto, config, config_schema, channel, thread_pool, buffer_pool, rt, systemd, version |
| 1 | services | log, log_rt, metrics, events, scheduler, health, alarms, audit, auth, assets |
| 2 | link | c2, zmq, http, hub |
| 3 | front ends | console, telnet, web |
| 4 | application | component, control, application (`src/app/`) |

When a lower module must call a higher one, give the lower module a callback
or an interface, and connect the two in the higher module.

A new module goes in the table in `tools/check-layers.py` and here. The move
to separate module folders (as in zephyrus) is a later step.

## F-012 Text (Z-021)

Comments, documentation, log messages, test names and commit messages use
ASD-STE100 Simplified Technical English: short sentences, active voice, one
word for one meaning. New and changed text obeys this rule.
