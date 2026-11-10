# GPIO and CAN

Two kinds of hardware almost every subsystem has: GPIO lines (power
switches, enables, interrupts, power-good) and a CAN bus. Both are in the
core library and use only the kernel: no library to add to the image.

## GPIO (`forge/gpio.hpp`)

forgecpp talks to the kernel's GPIO character device (`/dev/gpiochipN`,
uAPI v2, Linux 5.10 and later), the interface libgpiod 2 wraps. The old
sysfs interface (`/sys/class/gpio`) is not used: it is deprecated and lets
two programs drive the same line.

```cpp
forge::gpio::Output en("gpiochip0", "PAYLOAD_EN", {.consumer = "payload"});
en.set(true);

forge::gpio::Input pgood("gpiochip0", "PAYLOAD_PGOOD", {.edge = forge::gpio::Edge::both});
worker("pgood", [&](std::stop_token stop) {
    while (!stop.stop_requested()) {
        if (auto e = pgood.wait(100ms)) post([this, up = e->rising] { on_pgood(up); });
    }
});
```

- **A chip** is a path (`/dev/gpiochip0`), a name (`gpiochip0`) or the
  chip's label (`gpiodetect` lists them). Labels do not change when the
  probe order does: prefer them on a board with several chips.
- **A line** is its name from the device tree (`gpio-line-names`,
  `gpioinfo` shows them) or its offset (`"17"`). Names keep the code and the
  schematic in step.
- **Values are logical.** `active_low` inverts both ways: `set(true)` drives
  the line low, a rising edge is the line becoming active.
- **One owner per line.** A line is held while its `Output` or `Input`
  lives; a second request gets "line ... is in use by <consumer>". Name the
  consumer: `gpioinfo` shows who holds a line.
- **Edges** come from `Input::wait(timeout)`: call it in a `worker()` with a
  short timeout (it sees the stop request), and `post()` what it found to
  the main loop. `Event::seqno` counts the line's events: a gap means the
  kernel's queue overflowed.
- `debounce` (microseconds) is done by the kernel where the chip supports it.

**The simulated chip.** The chip `"sim"` (or `"sim:<name>"`) exists in every
process: any line name is there, outputs can be read and inputs driven with
`forge::gpio::sim::level()` and `sim::drive()`. Tests use it, and an
application can make it its default so that it runs on a PC
(`examples/payload-power`: `[payload].chip = "sim"`).

**Permissions.** `/dev/gpiochip*` belongs to root (or the `gpio` group on
some images). The generated systemd unit keeps devices reachable; a unit
with `USER` needs `SupplementaryGroups=gpio` or a udev rule.

## CAN (`forge/can.hpp`)

SocketCAN: raw frames for the data of the bus, ISO-TP channels for c2
commands.

### Raw frames

```cpp
forge::can::Socket bus("can0", {.filters = {{0x180, 0x780}}});   // ids 0x180..0x1FF
bus.send(forge::can::Frame::make(0x705, {state, uptime0, uptime1}));
forge::can::Frame f;
if (bus.receive(f, 100ms).ok()) { ... f.id, f.bytes() ... }       // ETIMEDOUT: nothing came
```

- Filters run in the kernel: the program only wakes for its frames.
- `{.fd = true}` sends and receives CAN FD frames (up to 64 bytes).
- `send()` waits at most its timeout when the interface's queue is full
  (nobody acknowledges on the bus: a missing terminator, a lonely node) and
  then answers `ETIMEDOUT`.
- One sender and one receiver at a time: give the socket to one `worker()`
  that does both (`examples/can-node`).

### Commands over ISO-TP

`[can].interface = "can0"` answers the application's commands on the bus,
as `[serial].device` does on a tty: the same c2 frames, the same zephyrus
framing, one framed piece per ISO-TP message
([protocol.md](protocol.md#on-a-can-bus)). Health shows `can` (failed while
the interface is missing; it is tried again every second), `forgectl info`
lists it with its ids.

```toml
[can]
interface = "can0"
tx_id = 0x701     # this program answers with this id
rx_id = 0x700     # and listens to this one
```

Calling a device on the bus from code:

```cpp
forge::serial::Link dev(forge::can::isotp_link({.interface = "can0", .tx_id = 0x600, .rx_id = 0x580}));
dev.call(cmd_id, request, reply);
```

and from Python (`PYTHONPATH=<build>/python`):

```python
from forge import serial, messages_pb2 as m
node = serial.Client.isotp("can0", tx_id=0x700, rx_id=0x701)
print(node.call(m.info, m.info_request(), m.info_reply))
```

### A bus on a PC

```sh
sudo modprobe vcan
sudo ip link add vcan0 type vcan && sudo ip link set vcan0 up
candump vcan0                                    # can-utils
```

ISO-TP needs `CONFIG_CAN_ISOTP` (a module in most distributions since
Linux 5.10). A Yocto image needs `kernel-module-can-isotp` (or the option
built in), and `iproute2` to bring the interface up:
`ip link set can0 up type can bitrate 500000`.

The generated systemd unit allows `AF_CAN` sockets
(`RestrictAddressFamilies`).

## What was tested where

The tests run on any Linux machine: GPIO on the simulated chip, CAN frames
and the ISO-TP link over `SOCK_SEQPACKET` socket pairs (they keep message
boundaries as CAN sockets do). The kernel paths (`/dev/gpiochip*` ioctls,
`CAN_RAW` and `CAN_ISOTP` sockets) build everywhere, but are to be checked
on a board ([ROADMAP.md](ROADMAP.md)).
