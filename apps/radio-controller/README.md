# radio-controller

A daemon that controls an **AD9364** transceiver (any AD936x on ADI's
`ad9361` driver) through **libiio** and moves IQ samples over **ZeroMQ**.
Built on forgecpp: the same controls are available from the web UI, the
console and a ZeroMQ control socket.

```
            ZeroMQ                                   libiio
clients  <-- PUB  tcp://*:5555  "iq" frames  <--  RX thread  <--  cf-ad9361-lpc (RX DMA)
clients  --> PULL tcp://*:5556  TX frames    -->  TX thread  -->  cf-ad9361-dds-core-lpc (TX DMA)
clients  <-> REP  tcp://*:5557  JSON control -->  main loop  -->  ad9361-phy (LO, rate, gain, ...)
browser  <-> HTTP :8080  /  spectrum + waterfall + controls,  /forge/  control panel
```

## Running

```sh
radio-controller --backend sim                          # simulated AD9364, no hardware needed
radio-controller --backend iio --uri local:             # on the board (in-kernel IIO devices)
radio-controller --backend iio --uri ip:192.168.2.1     # remote, via iiod on the board
radio-controller -c /etc/radio-controller/radio-controller.toml -i   # + backup console
```

Open `http://<host>:8080/` for the spectrum view. The daemon keeps running when
the radio is missing or the connection drops: it raises a `radio.device`
alarm and reconnects every 5 seconds.

## Data plane (ZeroMQ)

Every message is multipart: **topic**, **header** (JSON), **payload**.
Samples are `cs16`: interleaved little-endian int16 I/Q pairs at full scale
(±32767) in both directions. The daemon converts to and from the AD936x's
12-bit format.

**RX** (`[zmq].rx`, PUB, topic `iq`):

```json
{"seq": 1842, "timestamp_ns": 1762500000123456789, "samples": 65536,
 "format": "cs16", "sample_rate": 30720000.0, "center_freq": 2412000000.0}
```

`seq` increases by one per buffer; a gap means the subscriber was too slow
(PUB drops frames for slow subscribers instead of stalling everyone). Samples
are only copied out while someone is subscribed.

**TX** (`[zmq].tx`, PULL): a header and a payload of cs16 samples.
TX must be enabled first (`tx_enabled`); frames received while disabled are
counted as dropped.

- `{"format": "cs16"}`: streamed; the daemon regroups samples into DMA
  buffers of `buffer_size`. Send at least as fast as the sample rate, or the
  hardware underflows (`radio.tx_underflow` alarm).
- `{"format": "cs16", "cyclic": true}`: the hardware repeats this frame
  until `tx_stop` (test tones, beacons).

## Control plane (ZeroMQ REP, web, console)

JSON requests on `[zmq].control` (see `forge::zmq::serve_application`):

```json
{"cmd": "set", "id": "rx_lo_mhz", "value": 2437}
{"cmd": "get", "id": "rx_power_dbfs"}
{"cmd": "state"}
{"cmd": "press", "id": "tx_stop"}
{"cmd": "alarms"}
{"cmd": "ack", "id": "all"}
```

Replies have `"ok": true`, or `"ok": false` with `error` and `status`
(400 invalid value, 409 conflict, 502 rejected by the device, 503 radio not
connected).

| Control | Kind | Range / values |
|---|---|---|
| `rx_enabled` | toggle | RX streaming on/off |
| `rx_lo_mhz`, `tx_lo_mhz` | number | 70 – 6000 MHz |
| `sample_rate_msps` | number | 2.083334 – 61.44 MSPS (shared by RX and TX) |
| `rx_bw_mhz`, `tx_bw_mhz` | number | 0.2 – 56 MHz |
| `gain_mode` | select | `manual`, `slow_attack`, `fast_attack`, `hybrid` |
| `rx_gain_db` | slider | −3 – 71 dB; writable in `manual` mode (409 otherwise); shows the AGC gain |
| `tx_enabled` | toggle | accept TX frames |
| `tx_atten_db` | slider | 0 – 89.75 dB, 0.25 dB steps |
| `buffer_size` | number | samples per DMA buffer (restarts streaming) |
| `tx_stop`, `reconnect` | buttons | stop TX output / reopen the radio |
| `rx_power_dbfs`, `peak`, `rx_rate_msps`, `rx_overflows`, `subscriptions`, `tx_mode`, `tx_frames`, `tx_underflows`, `tx_dropped`, `temperature_c`, `connected`, ... | values | read-only telemetry |

The same commands are available in the console (`-i`):
`set rx_lo_mhz 2437`, `press tx_stop`, `controls`, `alarms`, `ack all`.

Alarms: `radio.device` (radio unavailable / connection lost),
`radio.rx_overflow`, `radio.tx_underflow` (both from the AXI core status
register), `radio.temperature` (above `temperature_alarm_c`).

## Client tools

`clients/` (installed to `share/radio-controller/clients`, need `pyzmq` and `numpy`):

```sh
./radioctl.py tcp://radio:5557 set rx_lo_mhz 2437      # control
./rx_capture.py tcp://radio:5555 --seconds 2 --out capture.cs16
./radioctl.py tcp://radio:5557 set tx_enabled true
./tx_tone.py tcp://radio:5556 --offset 1e6 --rate 30.72e6   # cyclic test tone
```

`radioctl.py` works with every forgecpp daemon that serves its controls over ZeroMQ.

## Configuration

See `config/radio-controller.toml`. With `state_file` set, values changed at
runtime (web, console, ZeroMQ) are restored on the next start.

## Simulator

`backend = "sim"` emulates an AD9364 for development and tests: a few
carriers at fixed frequencies (433.92, 868.3, 915, 1575.42, 2412, 2418.5,
2437, 5800 MHz), a noise floor and signal levels that follow the RX gain,
bandwidth filtering, AGC, real-time pacing (a slow reader gets overflows) and
TX→RX loopback (`[radio.sim].loopback`).

## Building

In-tree with forgecpp: `./install.sh --apps`. Standalone against an
installed forgecpp:

```sh
cmake -S apps/radio-controller -B build -DCMAKE_PREFIX_PATH=<forgecpp prefix>
cmake --build build && ctest --test-dir build
```

Without libiio only the simulator is built. The libiio backend targets the
libiio 0.x API (0.26 is bundled), which matches ADI's firmware and
distributions.
