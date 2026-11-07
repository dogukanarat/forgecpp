# channel-emulator

Radio channel impairments in the TX path, built on forgecpp: noise,
frequency offset and Doppler, phase noise, sample clock error, multipath,
fading, IQ imbalance, DC offset and interference, each on its own switch,
plus presets. It sits between a TX client (e.g. [qam-modem](../qam-modem/README.md))
and [radio-controller](../radio-controller/README.md):

```
qam-modem --TX (5586)--> channel-emulator --TX (5556)--> radio-controller --> TX1 --[att]-- splitter -+-- RX1 (qam-modem)
    ^                                                          |                                       +-- RX2 (spectrum-analyzer)
    +----------- RX1 "iq" (5555), control (5557) --------------+
spectrum-analyzer <---------- RX2 "rx2" (5555) ----------------+
```

The impairments are applied before the DAC, so every receiver behind the
splitter sees them: the modem's EVM and packet loss and the spectrum
analyzer's picture change together. Only the modem's `radio_tx` changes;
its RX, the radio's settings and the analyzer stay connected to
radio-controller directly. The emulator never sets the radio up, it only
reads its sample rate (every 5 s).

## Running

```sh
radio-controller -c radio-controller.toml --backend iio -D 'radio.rx_channels=[1]' -D 'radio.tap_rx=[2]'
channel-emulator -c channel-emulator.toml                             # http://<board>:8083/forge/
qam-modem -c qam-modem.toml -D modem.radio_tx=tcp://127.0.0.1:5586    # or radio_tx in qam-modem.toml
spectrum-analyzer -c spectrum-analyzer.toml --backend radio -D analyzer.center_hz=2100e6 -D analyzer.span_hz=3e6
```

Start them in any order (ZeroMQ connects when the other side comes up).
**Bypass** passes the samples through untouched, so the emulator can stay in
the path; to take it out, point the modem's `radio_tx` back at 5556.

Everything is a control (web page, `forgectl -e tcp://<board>:5587 set ...`,
telnet 2326):

```sh
forgectl -e tcp://board:5587 set preset awgn-20
forgectl -e tcp://board:5587 set frequency true
forgectl -e tcp://board:5587 set cfo_hz 3000
forgectl -e tcp://board:5587 set paths "0:0:0, 0.3:-6:90"
```

## Impairments

| Group | Control | |
|---|---|---|
| Level and noise | `gain_db` | path gain; SNR, DC and interference follow the signal |
| | `noise`, `snr_db`, `noise_bw_khz` | white Gaussian noise; the SNR holds in `noise_bw_khz` (750 = the modem's symbol rate: Es/N0) |
| Frequency and timing | `frequency`, `cfo_hz` | constant frequency offset |
| | `doppler_hz`, `doppler_period_s` | + sinusoidal Doppler (a satellite pass, a vehicle) |
| | `phase_noise`, `linewidth_hz` | Wiener phase noise of an oscillator with that linewidth |
| | `clock`, `clock_ppm` | transmitter sample clock error: the timing drifts inside a packet |
| Multipath and fading | `multipath`, `paths` | echoes `delay_us:gain_db:phase_deg, ...` (fractional delays, power normalized) |
| | `fading`, `fading_hz`, `rician_k_db` | flat Rayleigh / Rician fading, Jakes Doppler spectrum |
| Front end | `iq`, `iq_gain_db`, `iq_phase_deg` | IQ imbalance (an image at -f) |
| | `dc`, `dc_dbc` | DC offset |
| | `interference`, `interference_hz`, `interference_dbc`, `..._on_ms`, `..._off_ms` | a tone, continuous or pulsed |

Levels relative to the signal (SNR, dBc) use the signal's power **while it
is present**: the gaps between packets do not count, so a bursty sender gets
the SNR asked for while a packet is on the air, whatever its TX level.

Blocks go on with the sender's forge.v1 header (its timestamp), so
radio-controller's `tx_max_age_ms` measures the age from the sender.

The sample clock error keeps the number of samples (the modem paces its TX by
the RX samples): the delay drifts by `clock_ppm` inside a burst and goes back
to zero in a quiet gap, where the jump cannot be seen; a signal without gaps
has it forced back at ±32 samples (counted on the page).

## Presets and the 16QAM modem

`preset` sets everything at once (`custom` after any change). Measured with
qam-modem's waveform (1000-byte packets, SNR in the symbol rate), from
`tests/test_emulator.cpp`:

| Preset | What | Delivered | EVM |
|---|---|---|---|
| clean | nothing | 60/60 | −43.7 dB |
| awgn-30 / 25 / 20 / 15 / 10 | noise | 60 / 60 / 57 / 0 / 0 | −29.3 / −24.5 / −19.5 dB |
| offset-5k | 5 kHz offset, 30 dB | 60/60 | −29.3 dB |
| doppler | ±8 kHz over 20 s, 30 dB | 60/60 | −29.4 dB |
| oscillators | 1.5 kHz, 20 Hz linewidth, 20 ppm clock, 30 dB | 60/60 | −25.2 dB |
| indoor | echoes 0.1 µs −6 dB, 0.25 µs −12 dB, 30 dB | 60/60 | −28.3 dB |
| urban | echoes 0.5 µs −3 dB, 1.3 µs −8 dB, 2.7 µs −14 dB, 30 dB | 60/60 | −27.5 dB |
| mobile | Rician K 6 dB, 50 Hz fading, 25 dB | 56/60 | −24.1 dB |
| interference | −10 dBc tone at 400 kHz, 2 ms of 10 | 60/60 | −26.5 dB |
| bad-radio | IQ 1 dB / 3°, DC −25 dBc, 30 dB | 60/60 | −21.5 dB |

Corrupted packets are never delivered (CRC32). What the table says about the
modem: 16QAM needs about 20 dB Es/N0 for 1000-byte packets; frequency
offsets up to ±11.7 kHz and clock errors of tens of ppm are fine; phase noise
is fine up to a linewidth of about 50 Hz (200 Hz breaks it). The decision
feedback equalizer removes echoes up to about 4 µs (urban, indoor) and the
interfering tone; the gain and phase loops follow the fading (without noise
`mobile` delivers 100/100: what it loses at 25 dB are the fades' SNR dips).
Before the equalizer, urban delivered 0/60, interference 46/60 and mobile 49/60.

Through the whole chain (simulated radio, `tests/test_emulator.cpp`, and by
hand with `echo.py` at 2.2 Mbit/s): clean without loss; awgn-20 about 5-10 %
lost and an EVM of −19.5 dB at the modem; awgn-15 nothing; offset-5k no loss,
the modem measures 4990 Hz; urban no loss; interference 0.6 % lost; mobile
11 % lost (fades).

## CPU

At 3 MSPS on a PC core the emulator uses about 5 % of real time with noise
on (shown as **CPU** on its page, alarm above 90 %). Multipath costs in
proportion to the longest delay; the rest is cheap (table-based Gaussian
noise, rotators instead of sin/cos per sample). On a Zynq-7000 expect
roughly ten times as much.

## Files

```
../common/channel/      the impairments, plain C++ (Channel, Settings, presets)
src/emulator.*          the forgecpp application: PULL in, PUSH out, controls
tests/test_channel.cpp  every impairment measured on known signals
tests/test_emulator.cpp the application; the modem per preset; the whole chain
```
