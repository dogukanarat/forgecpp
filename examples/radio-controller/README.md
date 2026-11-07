# radio-controller

A daemon that controls an **AD936x** transceiver on ADI's `ad9361` driver
(AD9364 1R1T, AD9361/AD9363 2R2T) through **libiio** and moves IQ samples
as **forge.v1** protobuf messages over ZeroMQ.
Built on forgecpp: the same controls are available from the web UI, the
control panel, `forgectl`, the console and telnet.

```
            forge.v1 / ZeroMQ                                   libiio
clients  <-- PUB  [forge].data    5555  "iq" IqBlock     <--  RX thread  <--  cf-ad9361-lpc (RX DMA)   rx_channels
analyzer <--                            "rx1"/"rx2"      <--  RX thread      (shared channels)         tap_rx
clients  --> PULL [zmq].tx        5556  "tx" TxBlock     -->  TX thread  -->  cf-ad9361-dds-core-lpc   tx_channel
clients  <-> REP  [forge].control 5557  Request/Reply    -->  main loop  -->  ad9361-phy (LO, rate, gain, ...)
clients  <-- PUB  [forge].events  5559  state, alarms, logs, audit, health, metrics
browser  <-> HTTP :8080  /  spectrum + waterfall + controls,  /forge/  control panel
telnet   <-> 2323  console
```

## Running

```sh
radio-controller --backend sim                          # simulated AD9364, no hardware needed
radio-controller --backend iio --uri local:             # on the board (in-kernel IIO devices)
radio-controller --backend iio --uri ip:192.168.2.1     # remote, via iiod on the board
radio-controller -c /etc/radio-controller/radio-controller.toml -i   # + backup console
forgectl -e tcp://board:5557 controls                   # from anywhere: controls, alarms, logs, ...
telnet board 2323                                       # the console, remotely
```

Open `http://<host>:8080/` for the spectrum view. The daemon keeps running when
the radio is missing or the connection drops: it raises a `radio.device`
alarm and reconnects every 5 seconds.

## Data plane (forge.v1 streams)

Every message is multipart: **topic**, **`forge.v1.Header`** (source, host,
seq per topic, time, type, trace context), **body** (`radio.v1.*`, see
[`../common/ad936x/proto/radio/v1/radio.proto`](../common/ad936x/proto/radio/v1/radio.proto))
and the **samples**. Samples are `cs16`: interleaved little-endian int16 I/Q
pairs at full scale (±32767) in both directions. The daemon converts to and
from the AD936x's 12-bit format.

**RX** (`[forge].data`, PUB, stream `iq`, `radio.v1.IqBlock`): one block per
DMA buffer and channel it listens to:

```
IqBlock { channel: 1, sample_rate: 30720000, center_freq: 2412000000,
          samples: 65536, format: "cs16", rx_gain_db: 40, buffer: 1842 }
```

`Header.seq` increases by one per block; a gap means the subscriber was too
slow (PUB drops for slow subscribers instead of stalling everyone; receivers
count the gaps). Samples are sent from the pooled DMA copy without another copy
(zero copy) and only while someone is subscribed.

**TX** (`[zmq].tx`, PULL, topic `tx`, `radio.v1.TxBlock` + cs16 samples).
TX must be enabled first (`tx_enabled`); blocks received while disabled are
counted as dropped.

- `TxBlock { format: "cs16" }`: streamed; the daemon regroups samples into DMA
  buffers of `buffer_size`. Send at least as fast as the sample rate, or the
  hardware underflows (`radio.tx_underflow` alarm).
- `TxBlock { format: "cs16", cyclic: true }`: the hardware repeats this block
  until `tx_stop` (test tones, beacons).

## Channel roles (`[radio]`)

An AD9364 has RX1 and TX1; an AD9361/AD9363 in 2R2T mode has RX1, RX2, TX1
and TX2. Which of them this daemon uses, and which it shares with other
applications, is set in the configuration, according to the hardware:

```toml
rx_channels = [1]   # listens to: stream "iq" on [forge].data (IqBlock.channel)
tx_channel  = 1     # drives: samples from [zmq].tx
tap_rx      = [2]   # shares: streams "rx1" / "rx2" on [forge].data (e.g. the spectrum analyzer)
```

A role the transceiver cannot do (RX2 or TX2 on an AD9364) keeps the radio
closed with a `radio.device` alarm that names the setting; roles are fixed
until the next start (a reload keeps them).

In the AD936x, RX1 and RX2 share the RX LO, the sample rate and the RF
bandwidth and arrive in one DMA buffer (TX1 and TX2 share the TX LO). So
radio-controller stays the only owner of the transceiver; other applications
get the shared channels as streams `rx1` / `rx2` (`IqBlock` with `channel`,
`center_freq`, `sample_rate` and `rx_gain_db` of that channel).

- A block is the start of one DMA buffer (`min(tap_samples, buffer_size)`
  contiguous samples), sent at most `tap_hz` times per second and only while
  someone subscribes ("latest" policy: a slow reader gets the newest block,
  never a backlog). Shared channels are converted only when a block is due,
  so the cost is a few MB/s of copying.
- After a change of RX LO, rate or bandwidth, the next buffers are skipped
  (they were captured with the old LO).
- Gain is per channel: `gain_mode` / `rx_gain_db` (RX1), `rx2_gain_mode` /
  `rx2_gain_db` (RX2). TX attenuation applies to `tx_channel`.

Loopback test (e.g. TX1 → RX2): put an attenuator in the cable (30 dB or
more): the TX output (up to about +8 dBm) must stay well below the RX input's
maximum (about +2.5 dBm). The analyzer reports clipping as `ADC OVERLOAD`.

## Control plane (forge.v1, web, console, telnet)

Requests on `[forge].control` (`forge.v1.Request`, see the forgecpp README);
from the shell with `forgectl`:

```sh
forgectl -e tcp://board:5557 set rx_lo_mhz 2437
forgectl -e tcp://board:5557 get rx_power_dbfs
forgectl -e tcp://board:5557 state
forgectl -e tcp://board:5557 press tx_stop
forgectl -e tcp://board:5557 alarms / ack all / audit / health / streams / metrics
forgectl -e tcp://board:5557 lease group:Receiver 300  # keep others away from the RX settings
```

Errors carry a status: 400 invalid value, 403 role, 409 conflict (AGC owns
the gain, leased by someone else), 502 rejected by the device, 503 radio not
connected. The older JSON commands (`{"cmd": "set", "id": "rx_lo_mhz",
"value": 2437}`) are still accepted on the same socket.

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
| `rx2_gain_mode`, `rx2_gain_db` | select / slider | RX2 gain control, as for RX1 |
| `tx_stop`, `reconnect` | buttons | stop TX output / reopen the radio |
| `rx_power_dbfs`, `peak`, `rx_rate_msps`, `rx_overflows`, `subscriptions`, `hw_channels`, `rx_channels`, `tx_channel`, `tap_channels`, `tap_blocks`, `tap_subscriptions`, `tx_mode`, `tx_frames`, `tx_underflows`, `tx_dropped`, `temperature_c`, `connected`, ... | values | read-only telemetry |

The same commands are available in the console (`-i`, or `telnet board 2323`):
`set rx_lo_mhz 2437`, `press tx_stop`, `controls`, `alarms`, `ack all`,
`metrics`, `health`, `streams`.

Metrics (`/metrics`, `forgectl metrics`): `radio.rx.buffers`,
`radio.rx.samples`, `radio.rx.read_seconds` and the framework's own. Health
component `radio` (critical). The RX thread takes its scheduling from
`[threads.rx]` (e.g. `policy = "fifo"`, `priority = 50`, `cpus = [1]`).

Alarms: `radio.device` (radio unavailable / connection lost / channel roles
the transceiver cannot do),
`radio.rx_overflow`, `radio.tx_underflow` (both from the AXI core status
register), `radio.temperature` (above `temperature_alarm_c`).

## Client tools

`clients/` (installed to `share/radio-controller/clients`, need `pyzmq`,
`numpy` and `protobuf`; `forge.v1` comes with forgecpp and `radio.v1` with
radio-controller, both in site-packages; from build directories set
`PYTHONPATH=<forgecpp build>/python:<build>/examples/common/ad936x/python`):

```sh
forgectl -e tcp://radio:5557 set rx_lo_mhz 2437            # control
./rx_capture.py tcp://radio:5555 --seconds 2 --out capture.cs16
./rx_capture.py tcp://radio:5555 --topic rx2                # a shared channel
forgectl -e tcp://radio:5557 set tx_enabled true
./tx_tone.py tcp://radio:5556 --offset 1e6 --rate 30.72e6   # cyclic test tone
```

`forge.v1.framing` builds and parses the forge.v1 framing for your own scripts:

```python
from forge.v1 import framing
from radio.v1 import radio_pb2
topic, header, block, samples = framing.parse(sock.recv_multipart(), radio_pb2.IqBlock)
```

## Configuration

See `config/radio-controller.toml`. With `state_file` set, values changed at
runtime (web, forgectl, console, telnet) are restored on the next start.
`radio-controller --help-config` lists every key, `--check-config` validates
a file.

## Simulator

`backend = "sim"` emulates an AD9364 (or, with `[radio.sim].channels = 2`,
an AD9361 whose RX1 and RX2 see the same signals with independent noise) for
development and tests: a few
carriers at fixed frequencies (433.92, 868.3, 915, 1575.42, 2412, 2418.5,
2437, 5800 MHz), a noise floor and signal levels that follow the RX gain,
bandwidth filtering, AGC, real-time pacing (a slow reader gets overflows) and
a TX→RX cable (`[radio.sim].loopback`, from `loopback_tx` to `loopback_rx`). Generating samples costs CPU: on a PC it keeps up
with about 10 – 20 MSPS in a release build; use lower rates in debug builds.

## Building

As a forgecpp example (not installed):

```sh
./install.sh --examples
cd build/forgecpp/examples/radio-controller
./radio-controller -c radio-controller.toml
```

Standalone against an installed forgecpp; this installs the daemon, its
config, the systemd unit and the clients (the Yocto recipe builds it this way):

```sh
cmake -S examples/radio-controller -B build -DCMAKE_PREFIX_PATH=<forgecpp prefix>
cmake --build build && ctest --test-dir build && sudo cmake --install build
```

Without libiio only the simulator is built. The libiio backend targets the
libiio 0.x API (0.26 is bundled), which matches ADI's firmware and
distributions.
