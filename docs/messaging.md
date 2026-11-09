# Commands and streams

The message standard of every forgecpp program. Step by step: [commands.md](commands.md); the wire format: [protocol.md](protocol.md).

Messages are protobuf messages with a bound on every string, bytes and
repeated field (`[(nanopb).max_size = 64]`). nanopb turns them into plain C
structs: nothing allocates, and the same `.proto` file builds on a
microcontroller. forgecpp's own are in [`proto/forge`](../proto/forge)
(`messages.proto`, `hub.proto`; installed in `share/forgecpp/proto`).

**Commands** ([adding one, step by step](commands.md); the wire format:
[docs/protocol.md](protocol.md)). Every command is a request and a reply with a number (unique in
the program; `0x0001..0x00FF` are forgecpp's: ping, info, controls, get, set,
press, call, alarms, ack, logs, metrics, audit, lease, release, health,
streams, log level). One handler answers both ways in:

```proto
// proto/pump/pump.proto
enum msg_id { none = 0; flush = 0x0100; }
message flush_request { uint32 seconds = 1; }
message flush_reply { string message = 1 [(nanopb).max_size = 64]; }
```

```cpp
// CMake: forge_add_proto(pump ROOT proto PROTOS pump/pump.proto PYTHON ${CMAKE_BINARY_DIR}/python)
#include "pump/pump.forge.hpp"

// In the constructor (or setup_web()); the handler runs on the main loop.
command<pump_flush_request, pump_flush_reply>(pump_msg_id_flush, "flush", [this](const auto& req, auto& rsp) {
    if (req.seconds > 60) return forge::Status::error(forge::module_id::app, EINVAL, "at most 60 s");
    start_flush(req.seconds);
    return forge::pb::assign(rsp.message, "flushing");
});
```

- On a link (`[forge].control`, ZeroMQ REQ/REP) each message is one **c2
  frame**: an 8-byte header (version, flags, command number, sequence number,
  length) and the encoded request. The answer is a reply, an ACK (empty
  reply), a NACK with the 32-bit status (module and errno, numbered like
  Zephyr), or several replies flagged "more" for lists. These are the frames
  of zephyrus: a gateway can pass them to a device unchanged.
- On the web: `POST /forge/api/tmtc/<name or number>` with the request as
  JSON, answered with the reply as JSON (64-bit numbers as strings, enums by
  name, bytes as base64); a refusal is an HTTP error. `GET /forge/api/tmtc`
  lists the commands.

```cpp
forge::Remote radio("tcp://board:5557");              // get / set / press / call over the link
radio.set("rx_lo_mhz", 2412);
forge::zmq::Client c("tcp://board:5557");             // any command (timeout + reconnect)
pump_flush_reply rsp{};
c.call(pump_msg_id_flush, pump_flush_request{10}, rsp);
```

```python
from forge import c2, messages_pb2 as m              # forgecpp's Python package (site-packages)
link = c2.Client("tcp://board:5557")
link.call(m.set, m.set_request(id="rx_lo_mhz", value="2412"), m.set_reply)
```

| Endpoint (`[forge]`) | Socket | Carries |
|---|---|---|
| `control` | ROUTER (REQ clients) | c2 commands (forgecpp's and the application's) |
| `serial` | a tty (`/dev/ttyUSB0`) | the same commands on a UART, framed as zephyrus does ([protocol.md](protocol.md#on-a-serial-line)) |
| `events` | PUB | topics `state`, `alarms`, `log`, `audit`, `health`, `metrics` |
| `data` | PUB | the application's own streams (its messages, e.g. `radio.iq_block`) |

**Streams.** Published messages are multipart: **topic**,
**`forge.stream_header`** (source, host, pid, sequence number per topic, time,
body type, trace context), **body** and an optional **blob** (raw bytes such as
IQ samples, described by the body). Receivers count gaps in the sequence
numbers; the trace context continues across daemons, so a request can be
followed from the panel through every application it touches (audit records
carry it too).

```cpp
auto& samples = stream<pump_sample>("samples");                  // on [forge].data
auto& iq = stream<radio_iq_block>("iq", {"latest", 20});         // newest only, at most 20/s
if (samples.wanted()) samples.publish(msg);                      // any thread; wanted(): someone listens
iq.publish(block, buffer, buffer->data(), bytes);                // zero copy while `buffer` (shared_ptr) lives

forge::zmq::Receiver sub(forge::zmq::Receiver::Kind::sub, "tcp://board:5555",
    [this](forge::zmq::Message&& m) {
        auto s = m.as<pump_sample>();            // checks the header's type
        post([this, s] { use(s); });             // to the main loop
    }, {.topics = {"samples"}});
sub.lost();                                       // messages missed (sequence gaps)
```

```python
from forge import framing                         # PYTHONPATH=<build>/python from a build directory
topic, header, sample, blob = framing.parse(sock.recv_multipart(), pump_pb2.sample)
```

**forgectl** talks to any forgecpp application, over the link or HTTP:

```sh
forgectl -e tcp://board:5557 info                 # name, version, endpoints, uptime
forgectl -e tcp://board:5557 controls             # every control and command with its value
forgectl -e tcp://board:5557 set rx_lo_mhz 2412
forgectl -e tcp://board:5557 call scan from=2400 to=2480
forgectl -e tcp://board:5557 alarms / ack all / logs -n 50 / audit / health / streams / metrics
forgectl -e http://board:8080 trace -o trace.json # flight recorder for ui.perfetto.dev (HTTP)
forgectl -e tcp://board:5557 watch                # live events (state, alarms, logs, audit)
FORGE_ENDPOINT=tcp://board:5557 forgectl --json controls
```

Lower-level pieces: `Publisher`/`Stream` (thread-safe PUB with per-topic
subscriber counts), `Sender` (PUB/PUSH from one thread), `Receiver` (SUB/PULL
thread), `Server` (ROUTER + worker threads; `zmq::answer(registry, request)`
serves a `tmtc::Registry`), `c2` (the frame codec), `tmtc::Dispatcher` (for
other transports).
