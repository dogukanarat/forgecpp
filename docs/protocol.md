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
| the main loop is busy for longer than `[forge].request_timeout_ms`, or every worker is | `nack`, module tmtc, `EAGAIN` (11) |

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

`POST /forge/api/tmtc/<name or number>` takes the request as JSON and answers
with the reply as JSON (a list for replies in parts). A refusal is an HTTP
error with the status above. The JSON of a message is protobuf's: field names
as in the `.proto` (lowerCamelCase is accepted too), 64-bit integers as
strings, enums by name, bytes as base64. `GET /forge/api/tmtc` lists the
commands, `GET /forge/api/tmtc/<name>` describes their messages.

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
