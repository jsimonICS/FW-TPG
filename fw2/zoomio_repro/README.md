# ZoomIO waits run 26-40x slower than requested on FreeWili 2

> **Resolved by firmware v10 (2026-10-23).** After all three processors were
> updated to v10, both programs hold each level for 250.0 ms over 79 edges.
> That holds when polled every 6.4 ms and every 0.9 ms. Everything below was
> measured on `FW2 v04` and is kept as a record. The zone 6 requirement in
> "Other observations" still applies on v10: `repro.py` switches zone 6 on
> before running.

## Summary

On a FreeWili 2 running stock firmware on both CPUs, a ZoomIO program started
with Run ZoomIO (`s\b\r`) runs correctly but its `wait` delays are far too
long:

- `wait 250 ms` holds each level for about **6.6 s** (26.5x).
- `wait 37500000 cycles`, which is 250 ms at the documented 150 MHz, holds
  each level for about **9.9 s** (39.75x).

The two programs should take the same time, but the `ms` version runs in 2/3
of the time of the `cycles` version.

## Setup

- FreeWili 2, serial FX0105
- MAIN firmware: `FW2 v04`, as reported by the `?` command. That is older
  than the first public release (v07). v10 notes a fix for MAIN running from
  flash far below full speed.
- Stock DISPLAY firmware, no BSP app running
- Host: Windows 11, OneWili Python package from wilibsp `9e040a5`
  (`libs/onewili` at `b0eeccd`)
- Tested 2026-10-23

## Files

| File | Contents |
|---|---|
| `blink_ms.zio` | Square wave on GPIO 25 with `wait 250 ms` per level |
| `blink_cyc.zio` | Same program with `wait 37500000 cycles` per level |
| `repro.py` | Uploads a program, runs it through MAIN, and times GPIO 25 |

## Steps to reproduce

```
pip install pyfwfinder pyserial result
python repro.py blink_ms.zio
python repro.py blink_cyc.zio
```

Pass `--onewili <path>` if the OneWili Python package is not at
`../../wilibsp/libs/onewili/python` relative to the script.

The script switches power zone 6 on (`h\p\s 6 1`), stops any running
ZoomIO program (`s\b\s`) and drives GPIO 25 low (`i\g\l 25`). It then
uploads the program to `/appdata/zio_repro/` and runs it (`s\b\r <path>`).
Finally it polls `i\g\u` for 30 s and prints each edge on GPIO 25 and the
mean time per level.

## Results

| Program | Expected per level | Measured per level | Ratio |
|---|---|---|---|
| `blink_ms.zio` | 250 ms | 6610-6625 ms | 26.5x |
| `blink_cyc.zio` | 250 ms | 9937-9938 ms | 39.75x |

The run command returned `Ok` in 0.02 s for both programs, and no GPIO read
failed.

Each result comes from one 30 s run with 3-4 edges. The `ms` program gave
6.33-6.35 s per level in two earlier runs started from a display app instead
of the PC.

## What the numbers suggest (not confirmed)

- **The `ms` conversion seems to assume about 100 MHz.** 250 ms at 100 MHz is
  25 M cycles, and 25 M / 37.5 M = 2/3, which matches the measured ratio
  (6620 / 9938 = 0.666). The ZoomIO language reference says the default is
  150 MHz.
- **Counted cycles execute at about 3.8 M per second.** That is 37.5 M cycles
  in 9.94 s. The reference describes the long-wait loop as `addi` + `bne` at
  2 cycles per pass. The measurement works out to about 530 ns per pass,
  roughly 80 cycles at 150 MHz. That points to something slowing every
  instruction, such as code fetched from slow memory or core1 being stalled,
  rather than a wrong clock value.
- **The slowdown may depend on load.** The `ms` program ran about 4% slower
  when the PC polled every 5 ms (6.62 s) than when a display app polled every
  10 ms (6.34 s). This is only two runs, so treat it as a hint.

## Other observations

- **Zone 6 must be switched on by hand.** With stock firmware on both CPUs,
  MAIN refuses both `i\g\u` and `i\g\l` with `EPOWERZONE 6 FPGA` until zone 6
  is switched on. Nothing turns zone 6 on by default. Is gating header GPIO
  and ZoomIO on the FPGA zone intended?
- **One hang, not reproduced.** When `blink_cyc.zio` was started from a
  display RAM app instead of the PC, `s\b\r` never replied. MAIN then stopped
  answering both the display and the PC until a power cycle. The same program
  started from the PC ran normally. That is one attempt each.

## Measurement limits

GPIO 25 was read back with `i\g\u`, which reports MAIN's internal pin state.
That shows core1 is toggling the pin, but the header pin was not checked with
a scope. Polling gives a few milliseconds of resolution per edge, which is
small next to the multi-second errors above.

## Questions

1. What clock rate does the on-device compiler use to convert `ns`, `us`,
   `ms` and `s` waits to cycles?
2. Where does core1 execute ZoomIO code from, and is anything expected to
   stall it?
3. Is there a known-good way to get cycle-exact timing on hardware?
