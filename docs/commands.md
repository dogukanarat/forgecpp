# Adding a command

A command is a request and a reply with a number. One handler answers it on
a link (binary c2 frames, e.g. from a microcontroller or another program) and
on the web (JSON). The pump example (`examples/pump`) has one, `history`;
this page goes through it step by step. The wire format is in
[protocol.md](protocol.md).

Use a command when another program needs a typed request and reply. For a
value an operator changes on the panel, a control (`g.number(...)`,
`g.toggle(...)`) is simpler and gets a form, roles, leases and the audit
trail by itself.

## 1. The messages

In the application's `.proto` file. Every string, bytes and repeated field
needs a bound, so the message is a fixed-size C struct and nothing allocates:

```proto
syntax = "proto3";
import "nanopb.proto";
package pump;

// Command numbers: 0x0001..0x00FF are forgecpp's; the application starts at 0x0100.
enum msg_id {
    none = 0;
    history = 0x0100;
}

message history_request {
    uint32 count = 1;
}

message history_reply {
    repeated double pressure_bar = 1 [(nanopb).max_count = 32];
}
```

- `string name = 1 [(nanopb).max_size = 33];` holds 32 characters: nanopb's
  `max_size` counts the terminating NUL.
- `bytes data = 2 [(nanopb).max_size = 64];` holds 64 bytes.
- `repeated ... [(nanopb).max_count = N]` holds N values.
- No `map`, no `google.protobuf.*` types: the build refuses them, and a field
  without a bound, with the line to add.
- Changing a released message: add fields with new numbers; never renumber,
  retype or shrink a bound (a peer built with the old one would no longer fit).

## 2. The build

```cmake
forge_add_proto(pump ROOT pump PROTOS pump.proto PYTHON ${CMAKE_CURRENT_BINARY_DIR}/python)
```

This generates `pump.pb.h` / `pump.pb.c` (the structs: `pump_history_request`,
`pump_history_reply`, `pump_msg_id_history`), `pump.forge.hpp` (the names for
JSON) and, with `PYTHON`, `pump_pb2.py` for Python clients.

## 3. The handler

In the application's constructor (commands are fixed before the links start;
adding one later throws):

```cpp
#include "pump.forge.hpp"

Pump() : Application({"pump", "1.0.0", "Tutorial pump controller"}) {
    command<pump_history_request, pump_history_reply>(pump_msg_id_history, "history",
                                                      [this](const auto& req, auto& rsp) {
        const auto n = std::min<std::size_t>(req.count, history.size());
        return forge::pb::assign(rsp.pressure_bar, rsp.pressure_bar_count, std::span(history).last(n));
    }, forge::Role::viewer);  // a query: no role needed, nothing to audit
}
```

- **Roles and the audit trail**, as for the controls: by default the caller
  needs the `operator` role and every call is recorded (who, the request, the
  result; refusals too). A query passes `forge::Role::viewer` as the last
  argument: no role needed, no record, as `history` does. A command for admins
  only passes `forge::Role::admin`. Without `[auth]` everyone is admin; a link
  carries no credentials, so with `[auth]` on, link callers have
  `[auth].default_role`.
- The handler runs **on the main loop**, like the controls: it may read and
  change the application's state. If the loop is busy for longer than
  `[forge].request_timeout_ms`, the request is refused with `EAGAIN` and the
  handler never runs.
- It returns a `forge::Status`: `forge::Status()` for success, or
  `forge::Status::error(forge::module_id::app, EINVAL, "reason")`. Throwing
  `forge::Error(400, "...")` also works.
- `forge::pb::assign()` copies text, bytes or a list into a bounded field and
  refuses (`ENAMETOOLONG`, `E2BIG`) instead of cutting. `forge::pb::view()`
  reads a string field as a `std::string_view`.
- A reply that is too long for one message (a list) is sent in parts: register
  it with `commands().add_parts<Req, Rsp>(...)` and set `more` (see
  `src/app/commands.cpp`, e.g. "alarms").

## 4. Try it

```sh
forgectl -e tcp://localhost:5577 command                          # the list, with numbers
forgectl -e tcp://localhost:5577 command history '{"count": 3}'    # run it (JSON over HTTP)
curl -X POST http://localhost:8080/forge/api/commands/history -d '{"count": 3}'
```

The panel's **Commands** page (`/forge/#commands`) builds a form from the
request message and shows the reply.

From code:

```cpp
forge::zmq::Client link("tcp://localhost:5577");
pump_history_reply h{};
if (auto s = link.call(pump_msg_id_history, pump_history_request{3}, h); !s.ok()) {
    std::cerr << s.to_string() << "\n";
}
```

```python
from forge import c2
import pump_pb2
link = c2.Client("tcp://localhost:5577")
print(link.call(pump_pb2.history, pump_pb2.history_request(count=3), pump_pb2.history_reply).pressure_bar)
```

## 5. Test it

```cpp
TEST_CASE("pump: history over the link") {
    forge::testing::AppRunner<Pump> r({"-D", "forge.control=tcp://127.0.0.1:*", "-D", "http.enabled=false"});
    forge::zmq::Client link(r->endpoints().at("control"));
    REQUIRE(forge::testing::wait_until([&] { return r->invoke([&] { return r->history.size() >= 3; }); }));
    pump_history_reply h{};
    REQUIRE(link.call(pump_msg_id_history, pump_history_request{3}, h).ok());
    CHECK(h.pressure_bar_count == 3);
}
```

`examples/pump/test_pump.cpp` has it, next to the tests of the controls.
