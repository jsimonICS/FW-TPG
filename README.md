# FW-TPG

An RGB test pattern generator for classic 15 kHz arcade monitors, running on
the FreeWili OG and the FreeWili 2. It outputs 3-3-3 RGB (512 colors) plus
active-low composite sync on the 20-pin header. A simple resistor ladder turns
that into analog RGB for the monitor.

The signal is a non-interlaced "240p" field derived from RS-170/NTSC:

- 63.56 µs lines (15.73 kHz)
- 262 lines per field (60.05 Hz)
- Equalizing and serrated vertical sync pulses

It's for setting up and checking a monitor: convergence, geometry, purity,
color balance, gray scale and focus.

There are three implementations. All three use the same pins, the same ladder
board and the same set of patterns:

| | Board | How the signal is made |
|---|---|---|
| **OG** ([apps/tpg](apps/tpg)) | FreeWili OG | The main CPU's PIO and DMA play out a precomputed field buffer. A display app picks the pattern. |
| **ZoomIO** ([fw2/apps/tpg_zio](fw2/apps/tpg_zio)) | FreeWili 2 | Each pattern is a cycle-counted ZoomIO program, run by the stock MAIN firmware on its RISC-V core. Shipped as a display app; no firmware is replaced. |
| **FPGA** ([fpga/tpg](fpga/tpg), app in [fw2/apps/tpg_fpga](fw2/apps/tpg_fpga)) | FreeWili 2 | A custom design on the iCE40UP5K FPGA generates the signal in hardware. A display app loads it and selects patterns. |

## Patterns

1. Color bars (8 bars)
2. RGB gradient (8 steps per channel)
3. B&W gradient (8 steps)
4. Checkerboard
5. Crosshatch with dots
6. RGB crosshatch
7. Solid red
8. Solid blue
9. Solid green
10. Focus: a full screen of "H" characters (FPGA only)

The FPGA build can also overlay the pattern name and the line and field rates
as text. The BLUE button turns the overlay on and off.

On every build, GREEN steps to the next pattern and YELLOW to the previous
one.

## Comparison

| | OG (PIO + DMA) | FW2 ZoomIO | FW2 FPGA |
|---|---|---|---|
| **Signal source** | RP2040 PIO at 200 MHz, fed by DMA | ZoomIO interpreter on MAIN's RISC-V core1 at 150 MHz | iCE40UP5K at 25 MHz |
| **Timing step** | 5 ns | 6.67 ns (1 cycle) | 40 ns |
| **Timing accuracy** | Exact (every value is a multiple of 5 ns) | Line and field periods exact. Line starts can drift by up to 34 cycles (227 ns) within a field; the drift is corrected every field. Measured field period within ±35 ppm. | Line and field periods exact. Pulse widths rounded to 40 ns, at most 20 ns off (e.g. HSYNC 4.72 µs instead of 4.70 µs). |
| **Pin alignment** | Two PIO state machines started together | Pins change one `setio` at a time (2-3 cycles each) | All 10 outputs change on the same clock edge |
| **Patterns** | 9 | 9 | 10, plus text overlay |
| **Pattern change** | Buffers are rebuilt while DMA reads them, so one field can come out torn | The program is stopped and the next one started, so the output stops briefly | Clean, at the next field start |
| **At power-up** | Pattern on screen at boot | Launch the app | Launch the app (loads in about 270 ms, about 4 s the first time), or set **Default FPGA** to get color bars at boot without the app |
| **Firmware** | Replaces the OG's main and display firmware | Stock FW2 firmware v10 or newer; nothing is replaced | Stock FW2 firmware v11-preview.1 or newer (FPGA menu `h\f`); nothing is replaced |
| **Resources** | 2 PIO state machines, 2 DMA channels, about 40 KB of RAM | MAIN's ZoomIO core while running; programs must compile to 4096 bytes or less | 26% of the FPGA logic, 5 of 30 block RAMs; MAIN does no work once the design is loaded |
| **Keeps running** | Depends on the DMA interrupt being serviced every field (16.7 ms) | Depends on core1 running the program uninterrupted | Runs on its own once loaded |
| **Limitations** | The vertical sync pulses sit 1.52 µs off the hsync grid, because lines start at the front porch. Not an issue for monitors tested so far. | Rolling scan-line artifact (see [Known issues](#known-issues)). The 4 KB program limit constrains new patterns. The app changes MAIN's header level-shifter directions, and those settings persist after it exits. | Coarsest timing step. Needs preview firmware. Video timing and patterns are fixed at build time (changing them means rebuilding the bitstream). |
| **Checked on a CRT** | Yes | Yes, with the artifact below | Yes (patterns and all 9 color bits); the text overlay is new and still being checked |

In short:
- **OG:** the finest timing and the simplest setup, but it replaces the OG's
  firmware.
- **ZoomIO:** runs on stock FW2 firmware, but has the rolling artifact.
- **FPGA:** the cleanest output and the only one that can draw text, at the
  cost of a coarser timing step and preview firmware.

None of the timing differences above should be visible on a CRT. A 20 ns
difference is about a third of a pixel at 320 pixels across, and every value
is within normal RS-170 tolerance. The figures come from the designs and from
simulation, not from scope measurements.

## Known issues

### ZoomIO: rolling scan-line artifact

The ZoomIO build shows an artifact of scan lines that periodically rolls down
the image. The cause hasn't been found yet. The OG and FPGA builds don't show
it, so for clean output on a FreeWili 2, use the FPGA build.

<!-- VIDEO: paste the GitHub video link (https://github.com/user-attachments/assets/...) on its own line below -->

## Hardware

All three builds use the same GPIOs on the same header pins, so one cable and
ladder board fit both boards. Each channel is MSB first: bit 2 through 120 Ω,
bit 1 through 240 Ω and bit 0 through 470 Ω.

| Signal | GPIO | Header pin |
|---|---|---|
| B2 / B1 / B0 | 8 / 9 / 10 | 9 / 5 / 7 |
| G2 / G1 / G0 | 11 / 12 / 13 | 11 / 12 / 1 |
| R2 / R1 / R0 | 14 / 15 / 26 | 15 / 13 / 14 |
| CSYNC (active low) | 27 | 3 |

## Repository layout

| Path | Contents |
|---|---|
| [apps/tpg](apps/tpg) | OG build: `main/` (PIO, DMA, pattern buffers, timing), `display/` (pattern picker), `common/` (pattern table and main-to-display link, shared with the FW2 apps) |
| [fw2/apps/tpg_zio](fw2/apps/tpg_zio) | ZoomIO build: `gen_tpg_zio.py` generates the programs, `main.c` is the display app |
| [fpga/tpg](fpga/tpg) | FPGA design, testbench and text ROM generator. Its [README](fpga/tpg/README.md) covers the signal, registers and build. |
| [fw2/apps/tpg_fpga](fw2/apps/tpg_fpga) | Display app that embeds and loads the FPGA design |
| [fw2/apps/zio_probe](fw2/apps/zio_probe), [fw2/zoomio_repro](fw2/zoomio_repro) | ZoomIO timing probe and repro used during bring-up |
| `wiliOGbsp`, `wilibsp`, `wilifpga`, `freewili-gui` | Submodules: OG BSP, FW2 BSP, FPGA toolkit, FreeWili GUI |

## Building

Clone with submodules:

```sh
git clone --recursive https://github.com/jsimonICS/FW-TPG
```

**OG:** from the repo root, build with CMake and Ninja. The output is
`build/apps/tpg/tpg_main.uf2` (main CPU) and `tpg_display.uf2` (display CPU).
Flash each one to its CPU.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**FW2 apps:** from `fw2/`, build with the preset. The output goes to
`build-fw2/`. Then install from `wilibsp/`, replacing `COM14` with your
FreeWili 2's port:

```sh
cmake --preset target
cmake --build --preset target
```

```sh
python tools/fw.py install-app --folder Misc --port COM14 ../build-fw2/apps/tpg_zio/tpg_zio.uf2
python tools/fw.py install-app --folder Misc --port COM14 ../build-fw2/apps/tpg_fpga/tpg_fpga.uf2
```

The apps then appear under **Misc** in the FreeWili 2 app menu.

**FPGA app setup:** the FPGA app runs the wilifpga toolchain during the build,
and wilifpga only finds designs in `wilifpga/designs/`. Before the first
build, link the design in and install the toolchain. See
[fpga/tpg/README.md](fpga/tpg/README.md) for details.

```powershell
New-Item -ItemType Junction -Path wilifpga\designs\tpg -Target fpga\tpg
wilifpga\tools\fwfpga toolchain install
```

## License

MIT, see [LICENSE](LICENSE). The FPGA text overlay uses the 5×7 font from
`wilibsp` (MIT).
