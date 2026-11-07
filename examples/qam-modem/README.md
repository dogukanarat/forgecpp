# qam-modem

A 16QAM packet modem on an AD936x, built on forgecpp: UDP packets from
Ethernet go on the air, packets heard on the air go back out over Ethernet.
It does no device I/O itself: [radio-controller](../radio-controller/README.md)
owns the transceiver, the modem sends it TX samples and listens to its RX
stream, both over forge.v1 / ZeroMQ.

```
 remote PC                     board (Zynq + FMCOMMS5, chip A)
 ---------                     -----------------------------------------------------------------
 echo.py --UDP:6000-->  qam-modem --PUSH TxBlock--> radio-controller --> TX1 --[att]--+
                          ^                                                           | splitter
         <--UDP reply---  +--SUB "iq" (RX1)------- radio-controller <-- RX1 <--[att]--+
                                                                                      |
                     spectrum-analyzer <--"rx2" tap-- radio-controller <-- RX2 <--[att]--+
```

## Air interface

| | |
|---|---|
| Carrier | 2.1 GHz (`lo_hz`), RX and TX on the same LO |
| Sample rate | 3 MSPS, 4 samples per symbol |
| Symbol rate | 750 ksym/s |
| Pulse | root raised cosine, roll-off 0.35, 8 symbols long |
| Occupied bandwidth | 750 k × 1.35 = **1.01 MHz** (AD936x analog filters: 2 MHz) |
| Modulation | 16QAM, Gray coded (payload), QPSK (preamble, header) |
| Raw bit rate | **3 Mbit/s** |
| Net rate, 1472 B packets | **2.89 Mbit/s** (overhead: 64 + 24 + 16 symbols and the CRC per packet) |

The bandwidth: 3 MSPS is just above the AD9361's lowest rate without extra
FIR decimation (2.084 MSPS), 4 samples per symbol leave room for the RRC
skirt and for a frequency error, and 16QAM at 750 ksym/s gives 3 Mbit/s raw
for the 2 Mbit/s target with about 45 % margin. Doubling `sample_rate_hz`
doubles everything (and the CPU load).

Packet on the air:

```
| preamble: 64 QPSK  | header: 24 QPSK            | payload + CRC32, scrambled: 16QAM | gap   |
| 32-symbol PN, x2   | length, sequence, CRC16    | 2 symbols per byte                | 16 x 0|
```

Receiver, per packet ([src/qam.cpp](src/qam.cpp)):

1. **Detection and frequency offset**: Schmidl & Cox on the two halves of the
   preamble (running correlation and energies, threshold 0.6). The phase of the
   correlation is the frequency offset: ± 11.7 kHz can be corrected, i.e. ± 5.5 ppm at 2.1 GHz.
2. **Timing**: correlation with the known preamble waveform over ± 24 samples,
   a parabola through the peak gives the sample timing to a fraction of a sample.
3. **Matched filter at the symbol instants**: the RRC taps are recomputed for
   the packet's fractional delay, so no interpolator is needed.
4. **Gain and phase** from the preamble symbols, then a **decision-directed
   phase loop** (2nd order) through the header and payload.
5. Header CRC16 → length; payload 16QAM → descrambler → **CRC32**. Only
   packets with a good CRC are delivered: a weak link loses packets but never
   passes on corrupted ones.

The sample clock must be steady within a packet: true here (TX and RX of one
AD9361 share the clock). Two separate boards would need a timing loop too.

## Running

Three daemons on the board, the echo script on the PC.

```sh
# 1. the transceiver: chip A of the FMCOMMS5, RX1 for the modem, RX2 shared with the analyzer
radio-controller -c radio-controller.toml --backend iio -D radio.uri=local: \
    -D 'radio.rx_channels=[1]' -D 'radio.tap_rx=[2]' -D radio.tx_channel=1

# 2. the modem (sets radio-controller to 2.1 GHz, 3 MSPS, 2 MHz, manual gain, TX on)
qam-modem -c qam-modem.toml                      # web page: http://<board>:8082/

# 3. the spectrum analyzer on RX2: what is on the line
spectrum-analyzer -c spectrum-analyzer.toml --backend radio \
    -D analyzer.channel=2 -D analyzer.center_hz=2100e6 -D analyzer.span_hz=3e6   # http://<board>:8081/

# 4. on the PC: 2 Mbit/s of 1400-byte packets for 30 s
python3 clients/echo.py <board> --rate 2.0 --size 1400 --seconds 30
```

With radio-controller's simulator (below), 2.2 Mbit/s offered:

```
-> 127.0.0.1:6000  2.2 Mbit/s offered, 1400 B packets, 8.0 s
 time   sent    recv  missing%  rx Mbit/s   rtt ms (avg/max)
    1     197     190      3.55      2.128     35.6/  39.2
    2     393     387      1.53      2.206     35.5/  39.3
  ...
    8    1572    1565      0.45      2.195     35.5/  39.8

sent 1572, received 1572, lost 0 (0.000 %), corrupted 0, duplicates 0
throughput 2.201 Mbit/s (UDP payload), rtt median 35.5 ms, 99 % 38.4 ms
```

The modem answers to the address the packets came from (`udp_reply = ""`);
set `udp_reply = "host:port"` to send what it receives somewhere else.

Without a PC: tick **Test packets** on the web page (or
`forgectl -e tcp://<board>:5577 set test_mode true`). The modem then fills the
air with numbered test packets and counts what comes back (rate, lost, wrong
content): the link's capacity, about 2.88 Mbit/s.

On a PC, without hardware, the same with radio-controller's simulator (TX1 →
splitter → RX1 and RX2, 30 dB of loss, 1.2 kHz of frequency error):

```sh
cd build/forgecpp/examples
./radio-controller/radio-controller -c radio-controller/radio-controller.toml --backend sim \
    -D radio.sim.splitter=true -D radio.sim.loopback_db=-30 -D radio.sim.loopback_cfo_hz=1200 \
    -D 'radio.rx_channels=[1]' -D 'radio.tap_rx=[2]' &
./qam-modem/qam-modem -c qam-modem/qam-modem.toml &
./spectrum-analyzer/spectrum-analyzer -c spectrum-analyzer/spectrum-analyzer.toml --backend radio \
    -D analyzer.center_hz=2100e6 -D analyzer.span_hz=2.4e6 &
python3 ../../../examples/qam-modem/clients/echo.py 127.0.0.1 --rate 2.2 --seconds 10
```

## RF set-up (FMCOMMS5)

**Never connect TX to RX without attenuation.** The AD9361 RX input is
rated to about +2.5 dBm; the TX output at 0 dB attenuation is up to +7 dBm.

```
TX1 (chip A) --[30 dB]-- splitter (resistive, -6 dB) --[10 dB]-- RX1 (chip A)
                                                     +--[10 dB]-- RX2 (chip A)
```

* TX: `tx_level_dbfs = -12` and `tx_attenuation_db = 20` → about −20 dBm out.
* Path: 30 + 6 + 10 dB → about −66 dBm at RX1 and RX2. Anything from −80 to
  −40 dBm works; adjust with `tx_attenuation_db`.
* RX gain: manual (an AGC follows the bursts and hurts the first symbols).
  Watch **Receive / Level** on the modem page and set `rx_gain_db` so that it
  reads −15 … −30 dBFS: too low costs SNR, above −10 dBFS the peaks clip.
* Good link: **EVM below −25 dB** (shown as SNR above 25 dB). 16QAM with
  1400-byte packets needs about −20 dB; at −18 dB about half the packets fail
  their CRC.
* The spectrum analyzer should show a flat-topped signal about 1 MHz wide at
  2100 MHz; its gain is set on RX2 independently of the modem's RX1.

Chip B of the FMCOMMS5 (RX3/RX4, TX3/TX4) is not used: radio-controller
drives one AD9361. The libiio backend finds the FMCOMMS5's RX DMA
(`cf-ad9361-A`) on its own. Not yet tried on the hardware: the first run
should confirm the device names in the log (`radio: connected ...`).

## Configuration

[config/qam-modem.toml](config/qam-modem.toml), every key under `[modem]`.
The ones you may change:

| Key | Default | |
|---|---|---|
| `lo_hz` | 2.1e9 | carrier |
| `sample_rate_hz` | 3e6 | symbol rate = rate / `sps` |
| `rf_bandwidth_hz` | 2e6 | AD936x analog filters |
| `tx_attenuation_db`, `rx_gain_db` | 20, 30 | link budget (above) |
| `tx_level_dbfs` | −12 | TX RMS; also a control |
| `udp_listen`, `udp_reply` | 0.0.0.0:6000, "" | Ethernet side |
| `lead_ms` | 25 | how far TX runs ahead of RX: lower = less latency, more risk of underflows |
| `radio_buffer` | 16384 | radio-controller's buffer size (5.5 ms): smaller = less latency, more CPU |
| `configure_radio` | true | false: leave radio-controller's settings alone |

## Threads and CPU

| Thread | Work |
|---|---|
| `udp` | blocking `recvfrom` → TX queue (64 packets, more are dropped and counted) |
| `tx` | modulates 4096-sample blocks and pushes them to radio-controller; paced by the RX samples (one clock), not by a timer, so TX never drifts against the DAC |
| ZeroMQ receiver | demodulates every RX block and sends the packets out by UDP |
| main loop | controls, statistics, radio set-up (in the background) |

One second of signal (3 M samples) costs about 60 ms to modulate and 65 ms to
demodulate on a PC core (release build); expect around ten times that on a
Zynq-7000 Cortex-A9, i.e. well under one core each at 3 MSPS. Put the `tx`
thread on its own core with `[threads.tx]` if the board is busy.

## Files

```
src/qam.hpp/.cpp      the modem DSP, plain C++ (no forgecpp): framing, modulator, demodulator
src/modem.hpp/.cpp    the forgecpp application: UDP, radio-controller, controls, statistics
web/                  page with the constellation, rates and counters
clients/echo.py       traffic generator and checker for the remote PC (standard library only)
tests/test_qam.cpp    the DSP through an impaired channel: gain, phase, ±7 kHz frequency error,
                      fractional delay, noise, random block sizes; low SNR never delivers bad data
tests/test_modem.cpp  end to end: UDP -> modem -> radio-controller (simulated, splitter) -> modem
                      -> UDP at 2.3 Mbit/s, and the test mode
```
