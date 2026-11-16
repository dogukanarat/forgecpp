# FPGA side: the capture core (Arty Z7, Zynq-7000)

This is the design the program (`src/uio_source.cpp`) is written against. It
is a plan: the two blocks that define the data, the **input stage** and the
**change encoder**, are written and simulated (`rtl/`, `sim/`). The register
block, the FIFO, the DDR writer and the Vivado block design are described
below but not written yet.

## The idea: send changes, not samples

A logic analyzer that streams samples moves `channels × sample rate` bits all
the time: 4 channels at 100 MS/s are 50 MB/s, whether the lines move or not.
This core sends a 32-bit word only when an input changes (and a heartbeat now
and then), with the time since the previous word:

| Traffic on the four inputs | Changes/s | This core | Raw samples at 100 MS/s |
|---|---|---|---|
| idle lines | 0 | 4 kB/s (heartbeats, 1 ms) | 50 MB/s |
| UART 115200 baud, continuous | ≈ 60 000 | 240 kB/s | 50 MB/s |
| I2C 400 kHz, continuous | ≈ 1.2 M | 5 MB/s | 50 MB/s |
| SPI 10 MHz, 4 lines busy | ≈ 40 M | 160 MB/s | 50 MB/s |

So the capture length is set by how much the signals do, not by the sample
rate: a 16 MB buffer holds 4 million changes, minutes of a UART at 10 ns
resolution. The worst case is a fast clock on every line; above about
12 M changes/s raw samples would be smaller. The core drops words it cannot
store and says where (a gap word), so a too-busy capture is never silently wrong.

## Blocks

```
          Arty Z7 shield header                          Zynq PS (Linux)
 pins ─► la_input ─► la_encoder ─► FIFO ─► la_writer ═══ S_AXI_HP0 ═══► DDR ring buffer
 (4)     sync,        change words   4k×32   AXI4 bursts                 (16 MB, reserved)
         glitch       heartbeats             WR_INDEX                         ▲
           ▲             ▲                     │                              │ mmap (UIO map1)
           │ tick        │ lost / gap          ▼                              │
        divider ◄──── la_regs (AXI-Lite) ◄══ M_AXI_GP0 ══ logic-analyzer program (UIO map0)
                         │  THRESHOLD ─► PWM ─► shield comparator threshold
                         └─ IRQ ─────────────► IRQ_F2P[0] ─► /dev/uioN
```

| Block | What | State |
|---|---|---|
| `la_input` | 2-FF synchronizer per input; glitch filter: a new level passes after `GLITCH`+1 equal ticks (every edge then late by the same amount, intervals exact) | `rtl/la_input.v`, simulated |
| divider | one-clk `tick` every `DIVIDER`+1 clocks: 100 MHz / (D+1) | trivial counter |
| `la_encoder` | the words below; one word for any number of inputs changing on one tick; time words at least every `HEARTBEAT` ticks; drops when the FIFO is full, counts it, then writes a gap word | `rtl/la_encoder.v`, simulated |
| FIFO | 4096 × 32 bit BRAM (XPM_FIFO_SYNC): absorbs bursts while the writer waits for DDR | Xilinx primitive |
| `la_writer` | AXI4 master on S_AXI_HP0: 16-beat bursts (64 B) to `BUF_BASE + (WR_INDEX mod BUF_WORDS) × 4`, never across 4 kB; a partial burst after 1 ms idle; `WR_INDEX` advanced only after BRESP; stops reading the FIFO while `WR_INDEX − RD_INDEX > BUF_WORDS − 64` (the program has not read that far) | to write (≈ 250 lines), or Xilinx AXI DataMover (S2MM) |
| `la_regs` | AXI-Lite slave, register map below; IRQ when `IRQ_WORDS` new words or 1 ms after the first unread one | to write (≈ 150 lines) |
| threshold PWM | 12-bit PWM at 100 MHz / 4096 ≈ 24 kHz, `THRESHOLD`/4095 × 3.3 V after the shield's filter | trivial |

## The words

Little endian in memory, one per change (`src/wire.hpp` reads them):

```
 31 30 | 29 28 27 26 | 25 ............................. 0
 type  | levels D3‥D0 | delta: sample ticks since the previous word
```

| type | name | meaning |
|---|---|---|
| 0 | time | no change: a heartbeat, or a delta that would not fit in 26 bits |
| 1 | edge | one or more inputs changed on this tick; levels are the new ones |
| 2 | gap | words were dropped (FIFO full) before this one; delta still counts the time, levels are the current ones |
| 3 | start | first word after `CTRL.enable` rises: the levels at the start, delta 0 |

26 bits of delta are 0.67 s at 100 MHz; the heartbeat (default 1 ms) keeps
every delta far below that, and after a long drop the encoder writes time
words of the maximum first.

## Registers (AXI-Lite, 64 kB at 0x43C0_0000)

| Offset | Name | Access | Meaning |
|---|---|---|---|
| 0x00 | ID | RO | `0x4C41_vvvv`: "LA" and the core version |
| 0x04 | CTRL | RW | bit 0 enable (rising: a new capture, start word), bit 1 clear (self-clearing: indexes, counters, flags), bit 2 IRQ enable |
| 0x08 | STATUS | RO | bit 0 running, bit 1 overflow (words dropped, sticky), bit 2 FIFO overflow |
| 0x0C | DIVIDER | RW | sample tick = clock / (DIVIDER + 1) |
| 0x10 | GLITCH | RW | ignore pulses shorter than this many ticks (0: off) |
| 0x14 | HEARTBEAT | RW | a time word at least every N ticks (1 … 2²⁶−1) |
| 0x18 | BUF_BASE | RW | physical address of the ring buffer (64-byte aligned) |
| 0x1C | BUF_WORDS | RW | its size in words (a power of two) |
| 0x20 | WR_INDEX | RO | words written since enable (wraps at 2³²) |
| 0x24 | RD_INDEX | RW | words the program has read (it writes this) |
| 0x28 | LOST | RO | words dropped since enable |
| 0x2C | LEVELS | RO | the inputs now (bits 3..0), for a quick look |
| 0x30 | THRESHOLD | RW | comparator threshold, 0 … 4095 = 0 … 3.3 V |
| 0x34 | IRQ_WORDS | RW | interrupt after this many new words (and 1 ms after the first) |
| 0x38 | CLOCK_HZ | RO | the clock DIVIDER divides (100 000 000) |

The program (`src/uio_source.cpp`): clear, set DIVIDER, GLITCH, HEARTBEAT,
BUF_BASE/BUF_WORDS (from the UIO map), RD_INDEX = 0, THRESHOLD, IRQ_WORDS,
then enable. It reads from its index up to WR_INDEX, writes RD_INDEX, and
waits for the interrupt (write 1 to `/dev/uioN` to unmask it, then read).

## Linux

The buffer is reserved memory, and one UIO device maps the registers and the
buffer (Arty Z7-20: 512 MB DDR; the top 16 MB here):

```dts
/ {
    chosen {
        bootargs = "console=ttyPS0,115200 root=/dev/mmcblk0p2 rw rootwait uio_pdrv_genirq.of_id=generic-uio";
    };
    reserved-memory {
        #address-cells = <1>;
        #size-cells = <1>;
        ranges;
        la_buffer: buffer@1f000000 {
            reg = <0x1f000000 0x01000000>;   /* 16 MB */
            no-map;
        };
    };
};

&amba_pl {
    logic_analyzer: logic-analyzer@43c00000 {
        compatible = "generic-uio";
        reg = <0x43c00000 0x10000>,          /* map0: registers */
              <0x1f000000 0x01000000>;       /* map1: ring buffer */
        interrupt-parent = <&intc>;
        interrupts = <0 29 4>;               /* IRQ_F2P[0] = GIC SPI 61 */
    };
};
```

The node's name is the UIO name the program looks for
(`[analyzer] uio_device = "logic-analyzer"`), whatever `/dev/uioN` it gets.
`no-map` memory is mapped uncached: the program reads each word once, in order,
so this costs less than cache maintenance would. In Yocto
(`meta-xilinx`), the node goes in `system-user.dtsi` and
`KERNEL_MODULE_AUTOLOAD += "uio_pdrv_genirq"`.

## Vivado

1. Block design: ZYNQ7 Processing System with the Arty Z7 preset (board
   files from Digilent); enable M_AXI_GP0, S_AXI_HP0 (64 bit), IRQ_F2P,
   FCLK_CLK0 = 100 MHz.
2. Add the capture core (an RTL module of `la_input`, divider, `la_encoder`,
   FIFO, `la_writer`, `la_regs`) as a module reference: its AXI-Lite slave on
   GP0 at 0x43C0_0000, its AXI master on HP0, its IRQ on IRQ_F2P[0].
3. Constraints: from Digilent's `Arty-Z7-20-Master.xdc`, the shield pins used
   for D0‥D3 (`ck_io0` … `ck_io3`) and the threshold PWM (`ck_io4`), all
   `LVCMOS33`; the inputs need no timing constraints (they are synchronized).
4. Export the hardware (.xsa) for PetaLinux / meta-xilinx; the device tree
   above goes with it.

A faster tick: an MMCM makes 200 MHz for the capture clock (5 ns), and
CLOCK_HZ reports it; the program takes the sample rates from it.

## The shield (suggestions)

Per input, to measure 1.2 V … 5 V logic without hurting the board or the target:

- 1 kΩ series resistor and two Schottky clamps (BAT54S) to 3.3 V and GND:
  5 V signals are clamped with about 1.5 mA; with 5 pF of input the RC is 5 ns.
- 1 MΩ to GND, so an open input reads 0.
- A comparator with hysteresis and a push-pull 3.3 V output, fast enough for
  the sample rate: TLV3201 (40 ns, for ≤ 10 MHz signals), LMV7219 (7 ns) or
  TLV3501 (4.5 ns) for faster ones. Its threshold input from the FPGA's PWM
  through a two-pole RC (≈ 1 kHz) and a rail-to-rail op-amp buffer (e.g.
  MCP6002), shared by the four comparators.
- 33 Ω in series with each comparator output to the FPGA pin.

The program's "Logic level" sets the threshold: 1.2 V → 0.6 V, 1.8 V → 0.9 V,
2.5 V → 1.25 V, 3.3 V → 1.65 V, 5 V TTL → 1.4 V, or any value.

## How fast it can go

- The FPGA keeps up with any input at 100 MHz (one word per tick at most;
  DDR through HP0 takes several hundred MB/s).
- The limit is the CPU reading the words: on a 2.3 GHz Xeon the program
  records 72 M words/s and decodes SPI at 180 M changes/s
  (`tests/`); a 650 MHz Cortex-A9 reading uncached memory will do a fraction of
  that: measure it on the board (the panel shows words/s and words lost).
- Bursts larger than that are absorbed by the buffer: 16 MB are 4 M words.
  When it fills, the core drops words and the capture shows a hatched
  "data lost" stretch at that place; the alarm `la.lost` says so.

## Simulation

```sh
apt install iverilog
make -C fpga/sim
```

The testbench drives random levels (every clock, every third clock, with the
FIFO full now and then), decodes the words the way the program does, and
checks that every tick reads what was applied; and that the glitch filter
drops a 2-tick pulse and passes a 7-tick one 3 ticks late:

```
tick every clk, 10 % changes: 30000 ticks, 3061 words, 0 gap words, 0 lost
tick every 3rd clk, short heartbeat: 20000 ticks, 6482 words, 0 gap words, 0 lost
FIFO full now and then: 30000 ticks, 4948 words, 131 gap words, 4719 lost
glitch filter: checked
PASS
```
