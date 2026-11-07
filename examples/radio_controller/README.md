# radio-controller

A daemon that controls an **AD936x** transceiver on ADI's `ad9361` driver
(AD9364 1R1T, AD9361/AD9363 2R2T) through **libiio** and moves IQ samples
over **ZeroMQ**.
Built on forgecpp: the same controls are available from the web UI, the
console and a ZeroMQ control socket.

```
            ZeroMQ                                   libiio
clients  <-- PUB  tcp://*:5555  "iq" frames  <--  RX thread  <--  cf-ad9361-lpc (RX DMA)
clients  --> PULL tcp://*:5556  TX frames    -->  TX thread  -->  cf-ad9361-dds-core-lpc (TX DMA)
clients  <-> REP  tcp://*:5557  JSON control -->  main loop  -->  ad9361-phy (LO, rate, gain, ...)
analyzer <-- PUB  tcp://*:5558  RX2 blocks   <--  RX thread      (AD9361/AD9363, rx2_enabled)
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

## RX2 (AD9361/AD9363)

On a 2R2T transceiver RX2 can feed `spectrum-analyzer` while RX1/TX1 carry
your own traffic. In the AD9361, RX1 and RX2 share the RX LO, the sample
rate and the RF bandwidth, and both arrive in one DMA buffer. So
radio-controller stays the only owner of the transceiver: with `rx2_enabled`
it streams both channels and publishes RX2 on `[zmq].rx2`:

```json
{"seq": 812, "samples": 65536, "format": "cs16", "channel": 2,
 "sample_rate": 30720000.0, "center_freq": 2412000000.0, "rx_gain_db": 40.0}
```

- A block is the start of one DMA buffer (`min(rx2_tap_samples, buffer_size)`
  contiguous samples), sent at most `rx2_tap_hz` times per second and only
  while someone subscribes. RX2 is converted only when a block is due, so
  the cost is a few MB/s of copying.
- After a change of RX LO, rate or bandwidth, the next buffers are skipped
  (they were captured with the old LO).
- RX2 has its own gain control (`rx2_gain_mode`, `rx2_gain_db`); the analyzer
  sets them. LO and sample rate stay radio-controller's controls.
- On an AD9364 (or an AD9361 in 1R1T mode) `rx2_enabled` is refused (409) and
  a configured `rx2_enabled = true` raises the `radio.rx2` alarm.

Loopback test: cable TX1 to RX2 **through an attenuator** (30 dB or more):
the TX output (up to about +8 dBm) must stay well below the RX input's
maximum (about +2.5 dBm) and the RX2 gain must leave the ADC unclipped. The
analyzer reports clipping as `ADC OVERLOAD`.

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
| `rx2_enabled` | toggle | stream RX2 too (AD9361/AD9363; restarts streaming) |
| `rx2_gain_mode`, `rx2_gain_db` | select / slider | RX2 gain control, as for RX1 |
| `tx_stop`, `reconnect` | buttons | stop TX output / reopen the radio |
| `rx_power_dbfs`, `peak`, `rx_rate_msps`, `rx_overflows`, `subscriptions`, `rx_channels`, `rx2_blocks`, `rx2_subscriptions`, `tx_mode`, `tx_frames`, `tx_underflows`, `tx_dropped`, `temperature_c`, `connected`, ... | values | read-only telemetry |

The same commands are available in the console (`-i`):
`set rx_lo_mhz 2437`, `press tx_stop`, `controls`, `alarms`, `ack all`.

Alarms: `radio.device` (radio unavailable / connection lost), `radio.rx2`
(RX2 configured on a single-RX transceiver),
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

`backend = "sim"` emulates an AD9364 (or, with `[radio.sim].rx_channels = 2`,
an AD9361 whose RX1 and RX2 see the same signals with independent noise) for
development and tests: a few
carriers at fixed frequencies (433.92, 868.3, 915, 1575.42, 2412, 2418.5,
2437, 5800 MHz), a noise floor and signal levels that follow the RX gain,
bandwidth filtering, AGC, real-time pacing (a slow reader gets overflows) and
TX→RX loopback (`[radio.sim].loopback`, into the RX channel `loopback_rx`:
2 models a TX1 → RX2 cable). Generating samples costs CPU: on a PC it keeps up
with about 10 – 20 MSPS in a release build; use lower rates in debug builds.

## Building

As a forgecpp example (built when forgecpp has ZeroMQ, not installed):

```sh
./install.sh --examples
cd build/forgecpp/examples/radio_controller
./radio-controller -c radio-controller.toml
```

Standalone against an installed forgecpp; this installs the daemon, its
config, the systemd unit and the clients (the Yocto recipe builds it this way):

```sh
cmake -S examples/radio_controller -B build -DCMAKE_PREFIX_PATH=<forgecpp prefix>
cmake --build build && ctest --test-dir build && sudo cmake --install build
```

Without libiio only the simulator is built. The libiio backend targets the
libiio 0.x API (0.26 is bundled), which matches ADI's firmware and
distributions.
