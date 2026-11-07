"""Generate the TPG's ZoomIO programs, one per test pattern.

Port of apps/tpg/main/video_pattern.c (FreeWili OG, PIO + DMA) to ZoomIO on
the FreeWili 2 main CPU's RISC-V core1. The same timing (video_timing.h) and
the same nine patterns, in the same order as common/tpg_patterns.h -- but
instead of filling a DMA buffer, each pattern becomes a straight-line,
cycle-counted program: every segment is the setio statements that change pins
followed by a `wait` for the rest of the segment.

Why this is a generator and not nine hand-written .zio files:

  * Cycle accounting. setio costs 2 cycles (3 on GPIO 11), and only the pins
    that change get one. Loop bookkeeping costs cycles too, and a `wait` of 6+
    cycles runs 2 cycles long. The generator subtracts all of it from the
    segment's wait, so every line stays 9534 cycles (63.56 us at 150 MHz) and
    every field 262 lines. Every cost here was measured on FW2 v10 hardware.
  * Size. Identical lines become loops, and repeating groups of lines become
    nested loops, so even the crosshatch fits in a few hundred instructions.
  * Pin state across loop back-edges. A loop body's first segment is diffed
    against both the state on entry and the state at the end of the body, so
    every iteration drives the right pins.

Loops count down (`while cnt` / `let cnt--`): comparing a register against a
non-zero literal may use t6 as scratch, and t6 holds setio's GPIO_OUT_SET
address.

Outputs: one tpg_<name>.zio per pattern in --out-dir and, with --header, a C
header embedding them as string literals indexed by tpg_pattern_id_t.

--marker PIN adds a field marker for calibration: the program runs two fields
per loop and drives PIN high for the first, low for the second, so the pin's
level lasts exactly one field. Not for the shipped app.
"""
import argparse
import copy
import pathlib

CLOCK_HZ = 150_000_000   # ZoomIO's core1 clock (measured: 250.0 ms waits on FW2 v10)

# --- Timing, from apps/tpg/main/video_timing.h (ns) ---------------------------
H_TOTAL_NS = 63560
HSYNC_NS = 4700
BACK_PORCH_NS = 4700
FRONT_PORCH_NS = 1520
ACTIVE_NS = H_TOTAL_NS - HSYNC_NS - BACK_PORCH_NS - FRONT_PORCH_NS
BAR_COUNT = 8
GRID_CELLS = 8
GRID_LINE_NS = 320
GRID_PITCH_NS = 6540
GRID_DOT_NS = 300
GRID_ROW_SPACING = 40
HALF_LINE_NS = H_TOTAL_NS // 2
EQ_PULSE_NS = 2300
VSYNC_SERRATION_NS = 4700
LINES_PRE_EQ, LINES_VSYNC, LINES_POST_EQ = 3, 3, 3
LINES_PRE_BLANK, LINES_ACTIVE, LINES_POST_BLANK = 10, 241, 2


def cyc(ns):
    c, rem = divmod(ns * CLOCK_HZ, 1_000_000_000)
    if rem:
        raise ValueError(f"{ns} ns is not a whole number of cycles at {CLOCK_HZ} Hz")
    return c


LINE = cyc(H_TOTAL_NS)
ACTIVE = cyc(ACTIVE_NS)
BAR = ACTIVE // BAR_COUNT
assert BAR * BAR_COUNT == ACTIVE
GRID_GAP_NS = GRID_PITCH_NS - GRID_LINE_NS
assert GRID_CELLS * GRID_PITCH_NS + GRID_LINE_NS == ACTIVE_NS
assert (LINES_ACTIVE - 1) % GRID_ROW_SPACING == 0

# --- Pins ---------------------------------------------------------------------
# Exactly the OG's: the FW2 header puts the same GPIOs on the same physical
# pins, so one cable fits both. Header pin in the comments. MSB first within
# each channel, blue to red, as the ladder board is wired
# (apps/tpg/main/video_pattern.h): bit 2 on 120 ohm, bit 1 on 240, bit 0 on 470.
COLOR_PINS = {          # (channel, bit) -> GPIO
    ("b", 0): 10, ("b", 1): 9,  ("b", 2): 8,     # hdr 7, 5, 9
    ("g", 0): 13, ("g", 1): 12, ("g", 2): 11,    # hdr 1, 12, 11
    ("r", 0): 26, ("r", 1): 15, ("r", 2): 14,    # hdr 14, 13, 15
}
CSYNC_PIN = 27           # hdr 3; active low: 0 = sync asserted, 1 = idle
OUTPUT_PINS = sorted(set(COLOR_PINS.values()) | {CSYNC_PIN})
# The order a segment's setios run in. One pin per setio, so a color edge is
# smeared over up to ~19 cycles; going MSB to LSB across all three channels
# keeps the heavy bits of R, G and B within a few cycles of each other, so
# white lines don't grow a blue fringe on one side and a red one on the other.
SETIO_ORDER = [CSYNC_PIN] + [COLOR_PINS[(ch, bit)] for bit in (2, 1, 0) for ch in "rgb"]


def setio_cycles(pin):
    return 3 if pin == 11 else 2


# `wait N` of 6+ cycles compiles to a counted loop that runs 2 cycles long
# (measured on FW2 v10: N + 2 for N = 6 .. 8595); 1..5 are exact nops.
def wait_cycles(n):
    return n + 2 if n >= 6 else n


def wait_arg(cycles):
    """The `wait` argument that takes exactly `cycles` cycles."""
    if cycles in (6, 7):
        raise ValueError(f"no single wait takes {cycles} cycles (wait 5 = 5, wait 6 = 8)")
    return cycles - 2 if cycles >= 8 else cycles


# --- Cycle costs of loop bookkeeping (one cycle per instruction unless noted) -
LOOP_ITER = 1 + 1 + 2      # `while cnt` branch not taken, `let cnt--`, `jal` back (2)
LOOP_ENTRY = 1 + 2         # `let cnt = n`, final `while cnt` branch taken (2)
FOREVER_ITER = 2           # `while 1` has no header test: just the `jal` back


# --- Model --------------------------------------------------------------------
class Seg:
    """One stretch of constant output: color (r, g, b) at 0..7, sync, cycles."""

    def __init__(self, rgb, sync_idle, cycles):
        self.rgb, self.sync_idle, self.cycles = rgb, sync_idle, cycles
        self.absorb = 0          # loop-overhead cycles taken out of this wait
        self.extra = {}          # extra pin levels applied with this segment (marker)

    def key(self):
        return (self.rgb, self.sync_idle, self.cycles)

    def targets(self):
        t = {pin: (self.rgb["rgb".index(ch)] >> bit) & 1 for (ch, bit), pin in COLOR_PINS.items()}
        t[CSYNC_PIN] = 1 if self.sync_idle else 0
        t.update(self.extra)
        return t


class Line:
    def __init__(self, label, segs):
        self.label, self.segs = label, segs
        assert span(segs) == LINE, (label, span(segs))

    def key(self):
        return tuple(s.key() for s in self.segs)

    def last_seg(self):
        """The last segment outside any in-line loop: it runs once per line."""
        return [s for s in self.segs if isinstance(s, Seg)][-1]


class Repeat:
    """n passes over body: Lines and Repeats in a field, Segs within a Line."""

    def __init__(self, n, body):
        self.n, self.body = n, body

    def key(self):
        return ("R", self.n, tuple(s.key() for s in self.body))


def span(segs):
    return sum(s.cycles if isinstance(s, Seg) else s.n * span(s.body) for s in segs)


BLACK = (0, 0, 0)


def eq_line():
    p, h = cyc(EQ_PULSE_NS), cyc(HALF_LINE_NS - EQ_PULSE_NS)
    return Line("eq", [Seg(BLACK, False, p), Seg(BLACK, True, h), Seg(BLACK, False, p), Seg(BLACK, True, h)])


def vsync_line():
    lo, s = cyc(HALF_LINE_NS - VSYNC_SERRATION_NS), cyc(VSYNC_SERRATION_NS)
    return Line("vsync", [Seg(BLACK, False, lo), Seg(BLACK, True, s), Seg(BLACK, False, lo), Seg(BLACK, True, s)])


def porches():
    return [Seg(BLACK, True, cyc(FRONT_PORCH_NS)), Seg(BLACK, False, cyc(HSYNC_NS)),
            Seg(BLACK, True, cyc(BACK_PORCH_NS))]


def blank_line():
    return Line("blank", porches() + [Seg(BLACK, True, ACTIVE)])


def active_line(label, fill):
    # Repeating columns (crosshatch, checkerboard) become an in-line loop.
    return Line(label, porches() + fold(fill, key=Seg.key))


# --- Patterns (fills mirror video_pattern.c) ----------------------------------
BAR_RGB = [(7, 7, 7), (7, 7, 0), (0, 7, 7), (0, 7, 0), (7, 0, 7), (7, 0, 0), (0, 0, 7), (0, 0, 0)]


def solid(rgb):
    return [Seg(rgb, True, ACTIVE)]


def grad_step(i):
    return min(i // (LINES_ACTIVE // 8), 7)


def is_grid_row(i):
    return i % GRID_ROW_SPACING == 0


def is_dot_row(i):
    return i % GRID_ROW_SPACING in (GRID_ROW_SPACING // 2 - 1, GRID_ROW_SPACING // 2)


def crosshatch(i, colors, dots):
    if is_grid_row(i):
        return "grid", solid((7, 7, 7))
    dot_row = dots and is_dot_row(i)
    line, gap, dot = cyc(GRID_LINE_NS), cyc(GRID_GAP_NS), cyc(GRID_DOT_NS)
    side = (gap - dot) // 2
    assert 2 * side + dot == gap
    segs = []
    for col in range(GRID_CELLS):
        segs.append(Seg(colors[col % len(colors)], True, line))
        if dot_row:
            segs += [Seg(BLACK, True, side), Seg((7, 7, 7), True, dot), Seg(BLACK, True, side)]
        else:
            segs.append(Seg(BLACK, True, gap))
    segs.append(Seg(colors[GRID_CELLS % len(colors)], True, line))
    return ("dots" if dot_row else "ticks"), segs


def fill_for(pattern, i):
    if pattern == "COLOR_BARS":
        return "bars", [Seg(c, True, BAR) for c in BAR_RGB]
    if pattern == "RGB_GRADIENT":
        lvl = grad_step(i)
        col = ACTIVE // 3
        assert col * 3 == ACTIVE
        return f"rgb{lvl}", [Seg((lvl, 0, 0), True, col), Seg((0, lvl, 0), True, col), Seg((0, 0, lvl), True, col)]
    if pattern == "BW_GRADIENT":
        lvl = grad_step(i)
        return f"gray{lvl}", solid((lvl, lvl, lvl))
    if pattern == "CHECKERBOARD":
        row = min(i // (LINES_ACTIVE // 6), 5)
        return f"check{row % 2}", [Seg((7, 7, 7) if (row + c) % 2 == 0 else BLACK, True, BAR) for c in range(BAR_COUNT)]
    if pattern == "CROSSHATCH_DOTS":
        return crosshatch(i, [(7, 7, 7)], dots=True)
    if pattern == "RGB_CROSSHATCH":
        return crosshatch(i, [(7, 0, 0), (0, 7, 0), (0, 0, 7), (7, 7, 7)], dots=False)
    if pattern == "SOLID_RED":
        return "red", solid((7, 0, 0))
    if pattern == "SOLID_BLUE":
        return "blue", solid((0, 0, 7))
    if pattern == "SOLID_GREEN":
        return "green", solid((0, 7, 0))
    raise KeyError(pattern)


# Order and names match tpg_pattern_id_t (apps/tpg/common/tpg_patterns.h).
PATTERNS = ["COLOR_BARS", "RGB_GRADIENT", "BW_GRADIENT", "CHECKERBOARD", "CROSSHATCH_DOTS",
            "RGB_CROSSHATCH", "SOLID_RED", "SOLID_BLUE", "SOLID_GREEN"]


# --- Structure: run-length encode lines, then fold repeating groups -----------
def compress(lines):
    runs = []
    for ln in lines:
        if runs and runs[-1][0].key() == ln.key():
            runs[-1][1] += 1
        else:
            runs.append([ln, 1])
    items = [r[0] if r[1] == 1 else Repeat(r[1], [r[0]]) for r in runs]
    return fold(items)


def item_key(it):
    return ("L", it.key()) if isinstance(it, Line) else ("R", it.n, tuple(item_key(b) for b in it.body))


def fold(items, key=item_key):
    """Greedily replace k >= 2 back-to-back copies of a group with Repeat(k, group)."""
    out, i = [], 0
    keys = [key(it) for it in items]
    while i < len(items):
        best_p, best_k = 1, 1
        for p in range(1, (len(items) - i) // 2 + 1):
            k = 1
            while keys[i + k * p:i + (k + 1) * p] == keys[i:i + p]:
                k += 1
            if k >= 2 and p * k > best_p * best_k:
                best_p, best_k = p, k
        if best_k >= 2:
            out.append(Repeat(best_k, items[i:i + best_p]))
        else:
            out.append(items[i])
        i += best_p * best_k
    return out


def field(pattern):
    active = []
    for i in range(LINES_ACTIVE):
        label, fill = fill_for(pattern, i)
        active.append(active_line(label, fill))
    return ([Repeat(LINES_PRE_EQ, [eq_line()]), Repeat(LINES_VSYNC, [vsync_line()]),
             Repeat(LINES_POST_EQ, [eq_line()]), Repeat(LINES_PRE_BLANK, [blank_line()])]
            + compress(active) + [Repeat(LINES_POST_BLANK, [blank_line()])])


# --- Overhead placement ---------------------------------------------------------
# Loop bookkeeping is charged to a plain Line at the top level of the sequence
# it belongs to: such a line runs exactly once per pass of that sequence, the
# same number of times as the bookkeeping. normalize() guarantees one exists
# before any overhead is placed, so placement itself never restructures.
# Within a line, it goes to that line's last top-level segment (the back porch
# when the whole fill is a loop), which moves active video by a few cycles.
def unroll_one(rep, at_end):
    """Repeat(n, b) as a list with one iteration peeled off at the front or end."""
    rest = [Repeat(rep.n - 1, rep.body)] if rep.n > 2 else copy.deepcopy(rep.body)
    one = copy.deepcopy(rep.body)
    return rest + one if at_end else one + rest


def normalize(seq):
    i = 0
    while i < len(seq):
        if isinstance(seq[i], Repeat):
            if seq[i].n == 1:
                seq[i:i + 1] = seq[i].body
                continue
            normalize(seq[i].body)
        i += 1
    if not any(isinstance(it, Line) for it in seq):
        seq[-1:] = unroll_one(seq[-1], at_end=True)


def absorb(seq, cycles, prefer):
    lines = [j for j, it in enumerate(seq) if isinstance(it, Line)]
    j = min(lines, key=lambda j: abs(j - prefer))
    seq[j].last_seg().absorb += cycles   # last segment: the loop step follows it


def absorb_seg(segs, cycles, prefer):
    j = min((j for j, s in enumerate(segs) if isinstance(s, Seg)), key=lambda j: abs(j - prefer))
    segs[j].absorb += cycles


def place_overheads(seq):
    for i, it in enumerate(seq):
        if isinstance(it, Repeat):
            place_overheads(it.body)
            absorb(it.body, LOOP_ITER, len(it.body) - 1)
            absorb(seq, LOOP_ENTRY, i - 1)
        else:
            for k, s in enumerate(it.segs):
                if isinstance(s, Repeat):
                    absorb_seg(s.body, LOOP_ITER, len(s.body) - 1)
                    absorb_seg(it.segs, LOOP_ENTRY, k - 1)


def attach_first(seq, extra):
    """Apply extra pin levels with the first segment that runs in seq."""
    if isinstance(seq[0], Repeat):
        seq[0:1] = unroll_one(seq[0], at_end=False)
    if isinstance(seq[0], Repeat):
        attach_first(seq[0].body, extra)
    else:
        seq[0].segs[0].extra.update(extra)


# --- Emission ---------------------------------------------------------------------
class Emitter:
    def __init__(self):
        self.out, self.depth, self.setios, self.waits = [], 0, 0, 0
        self.last_wait = None    # (index in out, cycles) of a wait still open to merging

    def put(self, text):
        self.out.append("    " * self.depth + text)

    def put_wait(self, cycles):
        # Back-to-back waits (segments that change no pins) merge into one, so
        # the loop overhead in wait_cycles is paid once.
        if self.last_wait and self.last_wait[0] == len(self.out) - 1:
            cycles += self.last_wait[1]
            self.out.pop()
            self.waits -= 1
        self.put(f"wait {wait_arg(cycles)} cycles")
        self.waits += 1
        self.last_wait = (len(self.out) - 1, cycles)

    def seq(self, seq, state, dry=False):
        for it in seq:
            state = self.line(it, state, dry) if isinstance(it, Line) else self.repeat(it, state, dry)
        return state

    def line(self, ln, state, dry):
        if not dry:
            self.put(f"; {ln.label}")
        return self.segs(ln.segs, state, dry)

    def segs(self, segs, state, dry):
        for s in segs:
            if isinstance(s, Repeat):
                state = self.loop(s.n, lambda st, d, body=s.body: self.segs(body, st, d), state, dry)
                continue
            targets = s.targets()
            # CSYNC first, so the sync edge lands at the start of the segment;
            # then SETIO_ORDER, then anything else (the marker pin).
            change = sorted((p for p, v in targets.items() if state.get(p) != v),
                            key=lambda p: (SETIO_ORDER.index(p) if p in SETIO_ORDER
                                           else len(SETIO_ORDER), p))
            spent = sum(setio_cycles(p) for p in change)
            wait = s.cycles - spent - s.absorb
            if wait < 0:
                raise ValueError(f"{ln.label}: segment of {s.cycles} cycles cannot fit "
                                 f"{len(change)} pin changes plus {s.absorb} overhead")
            if not dry:
                for p in change:
                    self.put(f"setio {p}, {targets[p]}")
                    self.setios += 1
                if wait:
                    self.put_wait(wait)
            state = {**state, **targets}
        return state

    def repeat(self, rep, state, dry):
        return self.loop(rep.n, lambda st, d: self.seq(rep.body, st, d), state, dry)

    def loop(self, n, body, state, dry):
        end = body(state, True)
        # Pins that differ between entry and the end of an iteration are unknown
        # at the top of the body, so the first segment drives them every time.
        merged = {p: v for p, v in state.items() if end.get(p) == v}
        reg = f"a{self.depth}"
        if not dry:
            self.put(f"let {reg} = {n}")
            self.put(f"while {reg}")
            self.depth += 1
        body(merged, dry)
        if not dry:
            self.put(f"let {reg}--")
            self.depth -= 1
            self.put("endwhile")
        return end


def program(pattern, marker=None):
    # Overhead is placed field by field, so every field -- not just each pass
    # of the forever loop -- is exactly 262 lines.
    fields = [field(pattern)] if marker is None else [field(pattern), field(pattern)]
    if marker is not None:
        attach_first(fields[0], {marker: 1})
        attach_first(fields[1], {marker: 0})
    for f in fields:
        normalize(f)
        place_overheads(f)
    absorb(fields[-1], FOREVER_ITER, len(fields[-1]) - 1)
    body = [it for f in fields for it in f]

    em = Emitter()
    pins = OUTPUT_PINS + ([marker] if marker is not None else [])
    em.out += [f"; tpg_zio: {pattern.lower()} -- generated by gen_tpg_zio.py, do not edit",
               f"; {LINES_PRE_EQ + LINES_VSYNC + LINES_POST_EQ + LINES_PRE_BLANK + LINES_ACTIVE + LINES_POST_BLANK}"
               f" lines x {LINE} cycles at {CLOCK_HZ // 1_000_000} MHz"
               + (f"; field marker on GPIO {marker}" if marker is not None else "")]
    em.out += [f"setiodir {p}, 1" for p in pins]
    # GPIO_OUT resets low (black); only CSYNC needs its idle level up front.
    state = {p: 0 for p in pins}
    em.out.append(f"setio {CSYNC_PIN}, 1")
    state[CSYNC_PIN] = 1
    # The forever loop's body must start from the state it ends in.
    end = em.seq(body, state, dry=True)
    em.out.append("while 1")
    em.depth = 1
    em.seq(body, {p: v for p, v in state.items() if end.get(p) == v})
    em.depth = 0
    em.out.append("endwhile")
    return "\n".join(em.out) + "\n", em


def simulate(text, lines_to_run):
    """Run the emitted program under the same cost model and return the cycle
    count at the start of each line (the `; label` comments), first line = 0.
    An independent check of the generator's bookkeeping, not of the model."""
    prog = [ln.strip() for ln in text.splitlines()]
    match, stack = {}, []
    for pc, ln in enumerate(prog):
        if ln.startswith("while"):
            stack.append(pc)
        elif ln == "endwhile":
            match[pc] = stack[-1]
            match[stack.pop()] = pc
    regs, t, pc, starts = {}, 0, 0, []
    while len(starts) < lines_to_run:
        ln = prog[pc]
        op = ln.split()
        if ln.startswith("; ") and pc > 1:
            starts.append(t)
        elif op and op[0] == "setio":
            t += setio_cycles(int(op[1].rstrip(",")))
        elif op and op[0] == "wait":
            t += wait_cycles(int(op[1]))
        elif op and op[0] == "let" and op[1].endswith("--"):
            regs[op[1][:-2]] -= 1; t += 1
        elif op and op[0] == "let":
            regs[op[1]] = int(op[3]); t += 1
        elif op and op[0] == "while" and op[1] != "1":
            if regs[op[1]]:
                t += 1
            else:
                t += 2; pc = match[pc]
        elif ln == "endwhile":
            t += 2; pc = match[pc] - 1
        pc += 1
    return [s - starts[0] for s in starts]


ZIO_WINDOW = 4096   # bytes of compiled code MAIN will run (FW2 v10 rejects more)


def li_words(v):
    return 1 if -2048 <= v < 2048 or v & 0xFFF == 0 else 2


def code_bytes(text):
    """Upper bound on the compiled size, from the codegen in zoomio-language.md.
    Counts one parity nop per loop-form wait; against the sizes FW2 v10 reports
    that is high by about half an instruction per wait."""
    words = 0
    for ln in text.splitlines():
        op = ln.split(";")[0].split()
        if not op:
            continue
        if op[0] in ("setio", "setiodir"):
            words += setio_cycles(int(op[1].rstrip(",")))   # li mask + sw
        elif op[0] == "wait":
            n = int(op[1])
            words += n if n <= 5 else li_words(n // 2) + 2 + 1
        elif op[0] == "let" and op[1].endswith("--"):
            words += 1
        elif op[0] == "let":
            words += li_words(int(op[3]))
        elif op[0] == "while" and op[1] != "1" or op[0] == "endwhile":
            words += 1
    return 4 * words


def check(name, text, fields):
    n = fields * 262
    starts = simulate(text, n + 1)
    worst = max(abs(s - i * LINE) for i, s in enumerate(starts))
    for f in range(fields + 1):
        if starts[f * 262] != f * 262 * LINE:
            raise AssertionError(f"{name}: field {f} starts at {starts[f * 262]}, want {f * 262 * LINE}")
    return worst


def c_string(text):
    lines = text.splitlines()
    body = "\n".join('    "' + ln.replace("\\", "\\\\").replace('"', '\\"') + '\\n"' for ln in lines)
    return body


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out-dir", type=pathlib.Path, required=True)
    ap.add_argument("--header", type=pathlib.Path)
    ap.add_argument("--marker", type=int, help="field-marker GPIO (calibration builds only)")
    a = ap.parse_args()
    if a.marker in OUTPUT_PINS:
        ap.error(f"--marker {a.marker} is a video output; use a free header GPIO such as 25")
    a.out_dir.mkdir(parents=True, exist_ok=True)

    entries = []
    for name in PATTERNS:
        text, em = program(name, a.marker)
        worst = check(name, text, fields=4 if a.marker is None else 6)
        size = code_bytes(text)
        (a.out_dir / f"tpg_{name.lower()}.zio").write_text(text, newline="\n")
        entries.append((name, text))
        print(f"{name:16} {len(text.splitlines()):4} lines  {em.setios:4} setio  {em.waits:4} wait"
              f"  <= {size:4} B  line-start drift <= {worst} cycles, fields exact")
        if size > ZIO_WINDOW:
            # Marker builds run two fields per loop and may not fit; that only
            # costs a calibration run. A shipped program must fit.
            if a.marker is None:
                raise SystemExit(f"{name}: ~{size} bytes compiled, over the {ZIO_WINDOW}-byte ZoomIO window")
            print(f"{'':16} too big for the ZoomIO window; MAIN will refuse this one")

    if a.header:
        out = ["/* Generated by gen_tpg_zio.py -- do not edit. One ZoomIO program per",
               " * tpg_pattern_id_t, and the file name each is written to on the SD card. */",
               "#ifndef TPG_ZIO_SCRIPTS_H", "#define TPG_ZIO_SCRIPTS_H",
               '#include "common/tpg_patterns.h"', "",
               "/* Every GPIO the programs drive (setiodir ..., 1), CSYNC included. */",
               f"#define TPG_ZIO_PINS {{ {', '.join(str(p) for p in OUTPUT_PINS)} }}",
               f"#define TPG_ZIO_CSYNC_PIN {CSYNC_PIN}", "",
               "static const struct { const char *file; const char *zio; } TPG_ZIO[TPG_PATTERN_COUNT] = {"]
        for name, text in entries:
            out += [f"    [TPG_PATTERN_{name}] = {{", f'    "tpg_{name.lower()}.zio",', c_string(text), "    },"]
        out += ["};", "", "#endif", ""]
        a.header.parent.mkdir(parents=True, exist_ok=True)
        a.header.write_text("\n".join(out), newline="\n")


if __name__ == "__main__":
    main()
