# The command link: c2 frames and tmtc commands

What goes over `[forge].control` and `[forge].serial` (and, later, CAN), byte
for byte. It is the c2 frame of [zephyrus](https://github.com/embedded-aviation/zephyrus),
so a forgecpp program and a zephyrus device can talk to each other directly.
This page is enough to write a client or a device in any language; the
reference implementation is `include/forge/c2.hpp` and `src/tmtc.cpp`.

## The frame

```
 0         1         2                   4                   6                   8
+---------+---------+-------------------+-------------------+-------------------+-----------
| version | flags   | command number    | sequence number   | payload length    | payload ...
+---------+---------+-------------------+-------------------+-------------------+-----------
  1 byte    1 byte    2 bytes, big end.   2 bytes, big end.   2 bytes, big end.   0..65535 bytes
```

- **version** is `1`. A receiver drops a frame with another version.
- **length** is the number of payload bytes. A frame is exactly 8 + length
  bytes: a longer or shorter one is dropped, never cut.
- The **payload** is a protobuf message (the request or reply of the command).
- There is no sync word, checksum or address: the transport gives whole,
  checked frames. ZeroMQ carries one frame per ZeroMQ frame; a serial line
  adds its own framing and CRC ([below](#on-a-serial-line)); CAN will use ISO-TP.

### Flags

| Bit | Value | Name | Meaning |
|---|---|---|---|
| 0 | `0x01` | `req` | A request. It is answered. |
| 1 | `0x02` | `rsp` | A reply with a payload. |
| 2 | `0x04` | `push` | A report sent without a request. Nothing answers it. |
| 3 | `0x08` | `ack` | The request succeeded and there is nothing to send back. No payload. |
| 4 | `0x10` | `nack` | The request failed. Payload: the 4-byte status (below). |
| 5 | `0x20` | `proto` | The payload is a protocol message rather than command data. forgecpp passes it on and does not need it. |
| 6 | `0x40` | `more` | With `rsp`: more replies with this sequence number follow. |

A receiver decides what a frame is in this order: `req`, `nack`, `ack`,
`rsp`, `push`. A frame with none of them is ignored.

## A command

Every command is a pair of messages, a request and a reply, and has a number
that is unique in the program:

| Numbers | Whose |
|---|---|
| `0x0000` | none (never sent) |
| `0x0001`–`0x00FF` | forgecpp: ping, info, controls, get, set, press, call, commands, alarms, ack, logs, metrics, audit, lease, release, health, streams, log level; reports from `0x0080` (`proto/forge/messages.proto`) |
| `0x0100`–`0x7FFF` | the application, 256 numbers per module |
| `0xFF00`–`0xFFFF` | tests |

`get` and `set` carry a control's value as JSON text (`value`) and, for a
peer that does not parse JSON (a microcontroller), also in the `typed`
member that fits it: `number`, `flag` or `text`. A `set` with a `typed`
member set does not read `value`.

### Request and answers

The client picks a sequence number (it counts up and wraps at 65535) and
sends the request:

```
client  -> req          cmd=0x0105 seq=7  payload=<request message>
program -> rsp          cmd=0x0105 seq=7  payload=<reply message>
```

Every answer repeats the command number and the sequence number of its
request; a client drops answers that do not match. The answer is one of:

- **`rsp`**: the reply message.
- **`ack`**: success, the reply is empty (an empty protobuf message is zero
  bytes, so it is sent as an ACK without payload).
- **`nack`**: a refusal, with the status.
- **parts**: a list too long for one message (alarms, logs, controls) comes as
  several replies. All but the last carry `rsp|more`. The last carries `rsp`,
  or `ack` if it is empty. A `nack` in the middle ends the list.

```
client  -> req          cmd=0x0009 seq=8
program -> rsp|more     cmd=0x0009 seq=8  payload=<alarms 1..16>
program -> rsp|more     cmd=0x0009 seq=8  payload=<alarms 17..32>
program -> rsp          cmd=0x0009 seq=8  payload=<alarms 33..40>
```

On ZeroMQ all the answers to one request come in one multipart message, one
c2 frame per ZeroMQ frame.

What the program refuses before it calls a handler:

| Case | Answer |
|---|---|
| no command with that number | `nack`, module tmtc, `ENOENT` (2) |
| the number is a report (`push`) | `nack`, module tmtc, `ENOSYS` (88) |
| the request does not decode | `nack`, module pb, `EBADMSG` (77) |
| the reply does not fit its buffer | `nack`, module pb, `EMSGSIZE` (122) |
| the main loop is busy for longer than `[loop].request_timeout_ms`, or every worker is | `nack`, module tmtc, `EAGAIN` (11) |

A frame that does not decode, and any frame that is not a request, gets no answer.

## The status

A NACK carries a 32-bit status, big-endian:

```
 31 30                  16 15                   0
+--+----------------------+----------------------+
|1 | module (15 bits)     | code (16 bits)       |
+--+----------------------+----------------------+
```

`0` is success; bit 31 is set for every error. The **code** is an errno in
**Zephyr's numbering** (`lib/libc/minimal/include/errno.h`), whatever the
host is: Linux's `ENOSYS` is 38, on the wire it is 88. forgecpp converts both
ways (`forge::Status`, `errno_to_wire()`, `errno_from_wire()`).

| Module | Number |
|---|---|
| unknown (a plain errno) | 0 |
| app (zephyrus `MODULE_ID_APP`) | 64 |
| first id for modules outside zephyrus | 128 |
| forgecpp: c2, tmtc, pb, zmq, serial | `0x0100` .. `0x0104` |
| Nordic modem library (codes are not errnos) | `0x7FFD` |
| Zephyr kernel, drivers, libc | `0x7FFE` |

Common codes:

| Name | Wire code | HTTP (JSON path) | Typical reason |
|---|---|---|---|
| `EPERM` / `EACCES` | 1 / 13 | 403 | the caller's role is too low |
| `ENOENT` | 2 | 404 | no such command, control or alarm |
| `E2BIG` | 7 | 400 | more values than the field holds |
| `EAGAIN` | 11 | 503 | busy, try again |
| `EBUSY` | 16 | 409 | leased by someone else, wrong state |
| `EINVAL` | 22 | 400 | a value out of range, a bad argument |
| `ERANGE` | 34 | 400 | out of range |
| `EBADMSG` | 77 | 400 | the request does not decode |
| `ENOSYS` | 88 | 501 | not a command |
| `ENAMETOOLONG` | 91 | 400 | a text longer than its field |
| `ETIMEDOUT` | 116 | 504 | no answer |
| `EMSGSIZE` | 122 | 400 | a message too large |

The status has no text: the reason only travels on HTTP. On a link, log it on
the side that refuses. A handler's `forge::Error` goes out with the errno of
its HTTP status (400 → `EINVAL`, 403 → `EACCES`, 404 → `ENOENT`, 409 →
`EBUSY`, 503 → `EAGAIN`, 504 → `ETIMEDOUT`, others → `EIO`) and the module of
the command: **app** (64) for the application's commands, tmtc for forgecpp's
(`0x0001`–`0x00FF`). A `forge::Status` the handler returns keeps its own
module. forgecpp's own checks before a handler runs (the caller's role, a busy
main loop) are module tmtc.

## What is guaranteed

The links carry messages without a layer of their own for reliability; the
guarantees are those of the sockets and of c2, and they are these:

- **Commands are at most once.** A request has one answer (or several parts);
  without one in the client's timeout the client gives up (`ETIMEDOUT`) and
  does not know whether the handler ran. A request that must not run twice
  carries something to recognise it by (a token, a sequence of the caller's)
  in its message; c2 itself has no idempotency key. The server refuses, with
  `EAGAIN`, requests it cannot handle in time rather than queueing them for
  ever (`[forge].control_workers`, 256 waiting at most, `request_timeout_ms`).
- **Streams are at most once, in order per publisher.** A subscriber that
  cannot keep up loses messages (the publisher drops when a stream has
  `send_hwm` messages waiting, or all its streams `send_budget_bytes`; the
  socket drops at its own high-water mark); the `seq` in the stream header
  counts per topic, so the receiver sees the gap (`Receiver::lost()`). Within
  one publisher the numbers follow the order the messages were queued in, also
  when several threads publish one stream. Nothing is sent again.
- **Events (`[forge].events`) are the same as streams**; a subscriber that
  joins late gets nothing from before (the `state` topic sends every value
  once when the first subscriber appears).
- **Time in a header is the wall clock** (`time_unix_ns`); it moves with NTP.
  Use `seq` for loss and order, time for display.
- **A serial line adds**: a message is cut into at most 255 fragments of
  `frame_size - 8` bytes (236 with the default 244), so the longest message
  on a line is about 60 kB, below c2's 65 535-byte payload; a damaged or lost
  fragment loses that one message; there is no acknowledgement and no
  retransmission below c2.
- **There is no heartbeat on the command link**: a peer that is gone is
  noticed when a request times out. The `metrics` topic of `[forge].events`
  (every `[forge].metrics_interval_s`) and forge-hub's registration are the
  liveness signals a device has.
- **Addresses are static**: `[forge].control` and the other endpoints come
  from the configuration (and forge-hub lists them per application); there
  is no discovery on the network.

## Changing the messages

`proto/forge/API.json` records the messages of the last release;
`tools/proto-compat.py` (CI, `tools/check.sh`) refuses a change an old peer
would not understand:

- A field or an enum value keeps its number, name, type and label. A removed
  one leaves its number `reserved`. New fields, messages and commands are
  always fine.
- The nanopb bounds (`max_size`, `max_count`) are part of the wire contract:
  a peer decodes a message into a struct of the old size and refuses the whole
  message when a field carries more. So a bound may grow only in a message
  forgecpp receives (a `*_request`): old senders send less, and the new bound
  holds it. In a message a peer receives (a reply, a report, a stream header
  or body) a bound does not change: a zephyrus device built against the old
  bound would stop understanding forgecpp's answers. For more room, add a
  field or a command.

The same rule applies to an application's own messages when a device on a
link decodes them.

## On a serial line

`[forge].serial = "/dev/ttyUSB0"` answers the same commands on a UART (or USB
CDC), as zephyrus does on its links (`modules/lib/cobs`). The two ends are
equal: either may send requests, answers and reports. A c2 frame is one
*message*; a message is cut into *fragments* that fit one frame of at most
`[forge].serial_frame_size` bytes (244, zephyrus's
`CONFIG_ZEPHYRUS_COBS_FRAME_SIZE`):

```
0x00 | COBS( flags | seq | total | part of the message | CRC16 ) | 0x00
```

- **flags**: `0x01` first fragment, `0x02` last; **seq** counts from 0;
  **total** is the number of fragments (at most 255). A one-fragment message
  has flags `0x03`, seq 0, total 1.
- **CRC16** is CRC16-CCITT (polynomial `0x1021`, start `0xFFFF`, no
  reflection, no final XOR; `"123456789"` gives `0x29B1`) of the bytes before
  it, **little-endian**.
- **COBS** (Consistent Overhead Byte Stuffing) removes every zero byte, at a
  cost of one byte per 254, so `0x00` only marks a boundary. A receiver that
  starts in the middle or loses bytes finds the next frame at the next
  `0x00`. Every frame has both of its delimiters.
- A fragment out of order, a bad CRC, or a fragment after one was lost drops
  that message only; nothing is acknowledged or sent again. The c2 answer (or
  its absence: a timeout) tells the sender.
- Noise that holds a `0x00` opens a frame of noise; the delimiter that ends it
  (the leading one of the next real frame) does not start that frame, so the
  first fragment after such noise is lost with its message. zephyrus decides
  the same way (`tests/compat/zephyrus`, `tests/fuzz/fuzz_serial.cpp`); the
  sender's timeout and a new request cover it.
- A message is at most 16384 bytes on the forgecpp side (forgecpp's longest
  replies are about 12.5 kB); zephyrus's default is 4096
  (`CONFIG_ZEPHYRUS_COBS_MAX_MESSAGE_SIZE`): a device that asks for the long
  lists needs more.
- 8 data bits, no parity, one stop bit, no flow control;
  `[forge].serial_baud` (115200).

`tests/compat/zephyrus` checks this against zephyrus's own C code: the same
bytes for the same message, the same messages from the same damaged stream.

From code (to a device, or to a program with `[forge].serial`):

```cpp
forge::serial::Link link({.device = "/dev/ttyUSB0", .baud = 115200});
forge_info_reply info{};
auto s = link.call(forge_msg_id_info, forge_info_request{}, info);
```

```python
from forge import serial, messages_pb2 as m      # the standard library only, no pyserial
print(serial.Client("/dev/ttyUSB0").call(m.info, m.info_request(), m.info_reply).name)
```

`serial::Link` also answers requests from the other end (`on_request`, e.g.
with a `tmtc::Dispatcher`) and takes its reports (`on_push`).

## The same commands in JSON

`POST /forge/api/commands/<name or number>` takes the request as JSON and answers
with the reply as JSON (a list for replies in parts). A refusal is an HTTP
error with the status above. The JSON of a message is protobuf's: field names
as in the `.proto` (lowerCamelCase is accepted too), 64-bit integers as
strings, enums by name, bytes as base64. `GET /forge/api/commands` lists the
commands, `GET /forge/api/commands/<name>` describes their messages.

## Talking to it

```python
from forge import c2, messages_pb2 as m          # forgecpp's Python package
link = c2.Client("tcp://board:5557")
print(link.call(m.info, m.info_request(), m.info_reply).name)
```

```cpp
forge::zmq::Client link("tcp://board:5557");
forge_info_reply info{};
if (auto s = link.call(forge_msg_id_info, forge_info_request{}, info); !s.ok()) std::puts(s.to_string().c_str());
```

Answering commands on another transport: `forge::tmtc::Dispatcher(registry).handle(frame, send)`
takes one received frame and calls `send` for each answer.
