# spectrum-analyzer

A spectrum analyzer for the **AD936x** transceivers on ADI's `ad9361` driver
(AD9364, AD9361, AD9363), operated from the browser. Built on forgecpp: every setting is a
dashboard control, so the analyzer page, the generic control panel
(`/forge/`), the console and the ZeroMQ control socket all drive the same
instrument.

```
                      engine thread                                   main loop
cf-ad9361-lpc  -->  capture -> window -> FFT -> average  -->  frame  -->  SSE "frame"   --> browser  /
ad9361-phy     <--  LO / rate / bandwidth / gain per segment              ZeroMQ PUB     --> clients  tcp://*:5566
                                                                          ZeroMQ REP     <-> control  tcp://*:5567
```

## Running

```sh
spectrum-analyzer --backend sim                         # simulated AD9364, no hardware needed
spectrum-analyzer --backend radio                       # RX2 of radio-controller (AD9361/AD9363), see below
spectrum-analyzer --backend iio --uri local:            # on the board, owning the RX path
spectrum-analyzer --backend iio --uri ip:192.168.2.1    # remote, via iiod on the board
spectrum-analyzer -c spectrum-analyzer.toml -i          # + backup console (peaks, markers, set, press)
```

Open `http://<host>:8081/` (radio-controller has 8080). The simulator has carriers at 433.92, 868.3, 915,
1575.42, 2412, 2418.5, 2437 and 5800 MHz over a noise floor, so every
feature can be tried without hardware.

## Running next to radio-controller (AD9361/AD9363, `backend = "radio"`)

With a 2R2T transceiver both daemons run on the board at the same time:
radio-controller drives RX1/TX1 (your modem or test traffic) and the analyzer
looks at RX2.

```
                    ad9361-phy, cf-ad9361-lpc (RX1+RX2), TX DMA
                                     |
radio-controller  (owner: RX LO, sample rate, bandwidth, RX1/TX1, RX2 gain)
   [zmq].rx2  tcp://*:5558  RX2 blocks  ------>  spectrum-analyzer (backend = "radio")
   [zmq].control tcp://*:5557  <------  RX2 gain, rx2_enabled
```

- RX1 and RX2 of the AD9361 share the RX LO, the sample rate and the RF
  bandwidth, and arrive in one DMA buffer that only one process can open.
  So the analyzer does not tune: it shows the band radio-controller is tuned
  to (LO ± 80 % of the sample rate / 2) and you can zoom inside it (center,
  span, RBW, markers, everything else works). A window outside the band is
  replaced by the whole band; when radio-controller retunes, the display
  follows and the traces restart. The annotation bar shows
  `RX2 · LO … (radio-controller)`.
- The analyzer turns `rx2_enabled` on in radio-controller and owns RX2's gain
  (`gain_mode`, `rx_gain_db` act on RX2). dBm readings use the RX2 gain that
  comes with every block.
- The largest FFT is the block size (`rx2_tap_samples`, at most radio-controller's
  `buffer_size`; 65536 by default), so the finest RBW is about
  `2 × sample rate / 65536`.
- radio-controller missing, an AD9364, or no blocks for `radio_timeout_ms`
  raise `analyzer.device` with the reason; the analyzer retries every 2 s.

Loopback (TX1 → RX2 cable): put an attenuator in the cable (30 dB or more);
the TX output must stay well below the RX input's maximum level, and
`ADC OVERLOAD` on the display means the RX2 gain is too high.

With `backend = "iio"` the analyzer opens the RX DMA itself and can sweep
70 MHz – 6 GHz, but then radio-controller must not run (one buffer per
device).

## Features

| | |
|---|---|
| **Frequency** | Center / span or start / stop, presets (ISM 433/868/915, GPS L1, Wi-Fi 2.4/5 GHz), full span (70 MHz – 6 GHz), center ± one division, span ÷2 / ×2, last span |
| **Sweep** | Spans up to `sweep_rate × usable_fraction` (49 MHz by default) are captured in one go (*realtime*); wider spans are *swept*: the LO steps through the band and the flat middle 80 % of each capture is stitched together. Run / hold, single sweep, max update rate |
| **Bandwidth** | Auto RBW (span / 500) or manual RBW (1 k … 1 M keys or any value); the FFT size follows the RBW. Windows: rectangular, Hann, Hamming, Blackman-Harris, flat top. Video averaging (FFTs averaged per capture) |
| **Amplitude** | Reference level, 1/2/5/10/20 dB per division, auto scale, dBFS or dBm (`dBm = dBFS − RX gain + calibration`), manual gain or AD9364 AGC, ADC overload detection |
| **Traces** | Three traces: clear/write, max hold, min hold, average (N sweeps), view (freeze), blank. Detectors: peak, sample, average (power), negative peak; 401 – 4001 display points |
| **Markers** | Four markers, delta to marker 1, peak search, next peak, peak left / right, marker → center, **peak track** (active marker follows the strongest signal), **signal track** (retunes to keep it centered), markers on any trace, click / table to place |
| **Measurements** | Channel power (with power density) over a channel bandwidth at the center, occupied bandwidth (99 % or any percentage), peak table (top 10 above a threshold), limit line with an alarm |
| **Display** | Spectrum with graticule, trace legend, marker readout and peak labels; waterfall (turbo, viridis, inferno, grayscale; auto or fixed range); drag to zoom, wheel to zoom, double-click to center; keyboard shortcuts |
| **Export** | Traces as CSV (`/api/trace.csv`), screenshot as PNG, last raw capture as cs16 (`/api/iq.cs16`, realtime spans) |
| **Operation** | Device reconnect with an alarm, overload / limit alarms, settings persisted in `state_file`, systemd watchdog, temperature readout; captures are read in chunks of at most 256 k samples (1 MB DMA buffers), so a narrow RBW with heavy averaging does not exhaust CMA memory on a Zynq |

Keyboard: <kbd>P</kbd> peak search, <kbd>N</kbd> next peak, <kbd>←</kbd>/<kbd>→</kbd>
peak left/right, <kbd>C</kbd> marker → center, <kbd>Space</kbd> run/hold,
<kbd>R</kbd> restart traces, <kbd>A</kbd> auto scale, <kbd>+</kbd>/<kbd>−</kbd> zoom.

## How it measures

- **Scaling.** Each capture is windowed and transformed; power is
  `|X|² / (Σw)²`, so a full-scale complex tone reads 0 dBFS with any window
  (between two bins a tone reads up to 0.8 dB low with Blackman-Harris,
  1.4 dB with Hann and 3.9 dB with rectangular; flat top stays within about
  0.1 dB, so use it to read levels). Video averaging averages linear power.
- **RBW.** `RBW = ENBW × sample rate / FFT size`; the FFT size is the power of
  two that gets closest to the requested RBW without exceeding it (64 … 65536).
  The annotation bar shows the RBW actually used.
- **Detectors** reduce the FFT bins under each display point: peak (max),
  negative peak (min), sample (the bin at the point) or average (mean power).
  With fewer bins than points the values are interpolated.
- **Channel power** integrates the bins in the channel and divides by the
  window's ENBW, so it is the same with every window and counts noise and
  signals alike. **Occupied bandwidth** is the band holding the given share of
  the total power in the span, trimming equal halves from both ends.
- **dBm** needs a calibration: feed a known level (signal generator) and set
  `cal_offset_db` until the marker reads it. Use manual gain for comparable
  readings; with AGC the gain (and so the dBm offset) changes over time.

## Interfaces

**Web.** `/` analyzer, `/forge/` control panel, `/api/frame` latest sweep
(JSON; levels in tenths of a dB), `/api/trace.csv`, `/api/iq.cs16`
(headers `X-Sample-Rate`, `X-Center-Frequency`), `POST /api/tune`
`{"center_hz": ..., "span_hz": ...}` (one retune for both).

**ZeroMQ spectrum** (`[zmq].spectrum`, PUB, topic `spectrum`): a JSON header
and the live trace as little-endian float32 values:

```json
{"seq": 512, "start_hz": 2405000000.0, "stop_hz": 2445000000.0, "points": 1001,
 "unit": "dBFS", "rbw_hz": 48876.95, "format": "f32"}
```

```python
import json, zmq, numpy as np
s = zmq.Context().socket(zmq.SUB)
s.connect("tcp://board:5566"); s.subscribe(b"spectrum")
topic, header, payload = s.recv_multipart()
h = json.loads(header); levels = np.frombuffer(payload, "<f4")
freqs = np.linspace(h["start_hz"], h["stop_hz"], h["points"])
```

**ZeroMQ control** (`[zmq].control`, REP): the dashboard controls, as for
every forgecpp application:

```json
{"cmd": "set", "id": "center_mhz", "value": 915}
{"cmd": "set", "id": "trace2_mode", "value": "max_hold"}
{"cmd": "press", "id": "peak_search"}
{"cmd": "get", "id": "m1_mhz"}
{"cmd": "alarms"}
```

Control ids: `center_mhz span_mhz start_mhz stop_mhz preset full_span
rbw_auto rbw_khz vbw_averages window ref_level scale_db unit cal_offset_db
gain_mode rx_gain_db trace{1..3}_mode trace_average detector points
reset_traces active_marker marker_trace m{1..4}_on m{1..4}_mhz m{2..4}_delta
peak_search next_peak peak_left peak_right marker_to_center peak_track
signal_track measurement channel_bw_mhz obw_percent limit_on limit_level
peak_threshold run single update_hz reconnect`; read-only: `sweep_mode
rbw_actual fft_size sample_rate overload result sweep_time connected backend
temperature_c`.

Alarms: `analyzer.device` (radio not available, retried every 2 s),
`analyzer.overload` (ADC clipping), `analyzer.limit` (live trace above the
limit line).

## Configuration

See `config/spectrum-analyzer.toml`. Device settings (`backend`, `uri`,
`sweep_rate_hz`, `usable_fraction`, `settle_buffers`) are read at start-up;
the rest is the initial measurement setup, overridden by `state_file` once
the user changes something. Lower `sweep_rate_hz` when the radio is reached
over USB or the network: wide swept spans move `sweep_rate_hz` samples per
second per segment.

## Code

| File | |
|---|---|
| `src/spectrum.*` | planning (realtime / swept segments, FFT size for an RBW), bin stitching, detectors, traces, peak search, channel power, OBW: pure functions, unit tested |
| `src/engine.*` | the acquisition thread: owns the radio, retunes per segment, builds `Frame`s; reconnects on errors |
| `src/radio_tap.*` | `backend = "radio"`: RX2 blocks from radio-controller, RX2 gain over its control socket (fixed tuning) |
| `src/analyzer.*` | the forgecpp application: controls, markers, alarms, HTTP routes, ZeroMQ, persistence |
| `web/` | the analyzer page (canvas spectrum and waterfall, no dependencies) |
| `../common/ad936x` | AD936x access through libiio, the simulator, FFT and windows (shared with radio_controller) |

Tests (`tests/`) cover the DSP scaling, plans, detectors, traces, peaks and
measurements, the engine on the simulator (realtime and swept spans, markers,
peak track, single sweep, device errors, bounded DMA buffers), the RX2 mode
against a stand-in and against the real radio-controller (TX1 → RX2 loopback
on a simulated AD9361) and the whole application over ZeroMQ.
