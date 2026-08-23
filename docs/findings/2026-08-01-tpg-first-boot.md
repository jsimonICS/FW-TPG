# 2026-08-01 — `apps/tpg` first boot on a real FreeWili OG

What this records: getting the `tpg` (15.734 kHz RGB + composite-sync
test-pattern generator) main-CPU app to build and boot on real hardware for
the first time, and exactly what that run does and does not prove. Three
environment problems surfaced along the way that have nothing to do with
`apps/tpg` itself and will affect any app built on this machine — recorded
here since nothing else in the repo captures them yet.

## Environment problems fixed (not app-specific)

1. **`CMakePresets.json`'s pinned toolchain/ninja paths don't exist on this
   machine.** It hardcodes `PICO_TOOLCHAIN_PATH =
   $env{USERPROFILE}/.pico-sdk/toolchain/15_2_Rel1` and `CMAKE_MAKE_PROGRAM =
   $env{USERPROFILE}/.pico-sdk/ninja/v1.13.2/ninja.exe`; only `.pico-sdk/sdk/`
   was actually present. `fw build`/`fw.py` do not fall back the way
   `wilibsp`'s `fw.py` does. Worked around with a direct `cmake -S . -B build`
   invocation pointing at the machine's real ARM GNU Toolchain 14.2 rel1
   (`C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1`) and a
   winlibs-provided `ninja.exe`. Not fixed upstream — this repo's presets
   still assume versions that need installing (or the preset needs to learn
   to auto-discover, like `wilibsp`'s `fw.py` does).

2. **The Pico SDK's TinyUSB submodule was never checked out**, so the first
   configure warned "TinyUSB submodule has not been initialized" and every
   display-CPU-style app (anything linking `pico_stdio_usb`) failed to link
   with `cannot find -lpico_stdio_usb`. Fixed with `git submodule update
   --init lib/tinyusb` inside the *installed SDK* directory
   (`~/.pico-sdk/sdk/2.3.0`), not this repo — a one-time, per-machine SDK
   setup step, not a repo change.

3. **The locally-built `picotool` segfaults on `uf2 convert`.** This repo's
   CMake auto-fetches and builds `picotool` from source via `FetchContent`
   (`tools/Findpicotool.cmake`) when none is installed. Built here with the
   machine's mingw64 GCC 16.1.0, the resulting binary starts fine
   (`picotool version` works) but crashes with SIGSEGV/access-violation
   specifically on `uf2 convert` — reproduced 3x, isolated by manually
   replaying the full ninja link chain step-by-step until the exact failing
   subcommand was found (the link itself, `objdump`, `objcopy`, and
   `check_uf2_info.py` all succeeded standalone; only `picotool uf2 convert`
   crashed). This blocks **every** app in the tree, not just `tpg`, the first
   time anything needs a `.uf2` (alphabetically, `apps/lcd` hits it first).

   **Workaround used**: downloaded the official prebuilt Windows binary
   (`picotool-2.3.0-x64-win.zip` from
   `github.com/raspberrypi/pico-sdk-tools` release `v2.3.0-0`) and overwrote
   both `build/_deps/picotool-build/picotool.exe` and
   `build/_deps/picotool/picotool.exe` with it. That prebuilt binary
   converts the same ELF cleanly. This is a build-tree-local patch — a clean
   `build/` wipe reintroduces the crash. A durable fix would be pointing
   `PICOTOOL_FETCH_FROM_GIT_PATH` (or an equivalent override) at a
   pre-installed known-good `picotool`, or building the FetchContent copy
   with a different host compiler than this machine's default mingw64
   GCC 16.1.0 — root cause (miscompilation vs. a real picotool bug on this
   GCC version) was not investigated further.

## `fw flash`'s default UF2 path is wrong for any paired-folder app

`tools/fw.py`'s `uf2_path()` returns `build/apps/<app>/<app>.uf2`, using the
full target name (e.g. `tpg_main`) as the folder — but per this repo's own
"one folder, two targets" convention (AGENTS.md § App layout), the folder
drops the `_main`/`_display` suffix. The real path is
`build/apps/tpg/tpg_main.uf2`. This means `fw flash tpg_main` (and
presumably `fw flash template_main`, `fw flash ogvegas_main`, etc. — anything
whose folder name differs from its target name) fails with "not found" out
of the box. Worked around with the `--uf2` override
(`fw flash tpg_main --uf2 build/apps/tpg/tpg_main.uf2`). Not fixed upstream;
left for the maintainer to decide whether to special-case single-target
folders or fix `uf2_path()` generally.

## What was actually run and what it proves

```
fw build tpg_main            # (via the manual cmake invocation above)
fw flash tpg_main --uf2 build/apps/tpg/tpg_main.uf2
```

Flashed twice (once to confirm the copy succeeded at all, once more to
capture a clean boot). Both times:

- The board enumerated with USB VID:PID `093C:2054` and, per
  `fw flash`'s own port-matching output, the expected `FWOG main tpg 001`
  product string — confirming `fwog_main_app()`/`fwog_usb_ids()` wired up
  correctly for a brand-new app.
- Opened the resulting `USB Serial Device (COM34)` at 115200 8N1 (DTR/RTS
  asserted) and captured a continuous `[tpg_main] alive` heartbeat with
  **no reset/watchdog-loop pattern** over multiple observation windows
  (tens of seconds total, well past the 8.3 s watchdog period).

**What that heartbeat actually proves**: `board_init()`,
`fwog_display_update_run()`, the three-surface `fwog_io_dir_apply()` call
(GPIO 8-15/26-27 direction, which requires the display CPU up and
answering), PIO program loading (`pio_add_program` + the `hard_assert(off8
>= 0 && off2 >= 0)` guard), DMA channel claim/configure, and
`tpg_pattern_build()`'s own internal consistency check
(`hard_assert(bd.idx == TPG_FIELD_WORDS)`) all completed without hanging or
panicking, on a real board. Any of those failing would have either wedged
before the main loop (no heartbeat at all) or panicked (an SDK panic halts;
combined with the 8.3 s watchdog that means a reset loop, which would show
up as the 10-second "display: ... io_dir: ..." announce line repeating
forever instead of settling into "alive").

**What it does NOT prove** — this is the important limit, not a footnote:

- Whether `fwog_io_dir_apply()` actually returned `FWOG_IO_OK` rather than
  failing gracefully (`io_ok = false`, which is a valid non-crashing
  outcome the code handles). The first 10-second window that prints
  `io_dir: ok`/`FAILED` was missed both times — by the time the capture
  script opened the port, the board had already been running past that
  window. Whoever next flashes this should watch from the very first
  instant (e.g. hold the port open across the flash cycle) to catch it.
- Anything about the actual analog signal: composite sync timing accuracy,
  whether the color-bar bit patterns reach the physical pins correctly
  through the breakout header's level-shifting path, or whether a real
  15 kHz monitor can lock to it. **None of this was checked** — it needs a
  scope on GPIO 8-17/26-27 and/or the arcade monitor itself, neither of
  which this session had access to. Treat the video-generation design
  (`video_timing.h`, `video_pattern.c`, `tpg_video.pio`) as unverified
  until someone puts a probe on it.

## Reference: pin assignment used

| Signal | GPIO | PIO group |
|---|---|---|
| R2 R1 R0 | 8 9 10 | bus8 (`out pins, 8`, base=8) |
| G2 G1 G0 | 11 12 13 | bus8 |
| B2 B1 | 14 15 | bus8 |
| B0 | 26 | bus2 (`out pins, 2`, base=26) |
| CSYNC | 27 | bus2 |

CSYNC is active-low (0 = sync asserted). GPIO 16/17 (the I2C0 breakout pair)
were deliberately excluded — see `apps/tpg/main/video_pattern.h`'s pin-table
comment for why.
