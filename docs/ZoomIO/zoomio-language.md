# ZoomIO Language

A small, growing language that compiles to RV32I machine code.
Statements are one per line; blank lines and comments are ignored.

## Comments

```
; comment to end of line
// also a comment
```

## Number literals

- Decimal: `42`
- Hex: `0x12345`
- Binary: `0b1010`
- Negative: prefix with `-`, e.g. `-1`, `-0x10`

Values must fit in 32 bits (signed or unsigned).

## Registers

The 32 general-purpose RISC-V registers can be referred to by index or by ABI name.

- Numeric: `x0` .. `x31`
- ABI: `zero ra sp gp tp t0..t6 s0..s11 a0..a7` (`fp` is an alias for `s0`)

Writing to `x0` (`zero`) is an error.

## Statements

### let

Assigns to a register. The right-hand side is one of:

- a single primary (constant or register), or
- a binary expression `<primary> <op> <primary>` (no nesting, no parens).

```
let <reg> = <int>
let <reg> = <reg>
let <reg> = <primary> <op> <primary>
```

Examples:

```
let x1 = 7             ; load immediate
let x2 = x1             ; copy register
let s0 = a0 + a1        ; reg-reg add
let s5 = a0 + 100       ; reg-imm add (uses addi)
let s6 = a0 - 5
let s2 = a0 * a1        ; uses M-extension MUL
let t0 = a1 < a0        ; comparison: 1 if true, 0 if false
let t6 = 100 * 100      ; both operands literal -> folded at compile time
```

When both operands are literals, the result is folded at compile time and
the destination is loaded with the constant — no arithmetic instruction is
emitted.

#### Operators

| Op | Reg-Reg | Reg-Imm |
|----|---------|---------|
| `+` | `add` | `addi` (12-bit signed) |
| `-` | `sub` | `addi -imm` (reg - imm) |
| `*` | `mul` | use a register |
| `/` | `div` | use a register |
| `%` | `rem` | use a register |
| `&` | `and` | `andi` |
| `\|` | `or` | `ori` |
| `^` | `xor` | `xori` |
| `<<` | `sll` | `slli` (shift 0..31) |
| `>>` | `sra` (arithmetic right) | `srai` (shift 0..31) |
| `>>>` | `srl` (logical right) | `srli` (shift 0..31) |
| `<` | `slt` | `slti` |
| `<=` | `slt` + `xori 1` | use a register |
| `>` | `slt` (operands swapped) | use a register |
| `>=` | `slt` + `xori 1` | use a register |
| `==` | `sub` + `sltiu rd,rd,1` | `xori` + `sltiu` |
| `!=` | `sub` + `sltu x0,rd` | `xori` + `sltu` |

Where the right-hand column shows "use a register", that means there is no
direct RV32I encoding and the operand has to be materialized into a register
first:

```
let tmp = 100
let s0 = a0 > tmp
```

Notes:

- `*`, `/`, `%` require the M-extension at runtime (the simulator implements it).
- `/` and `%` follow RISC-V semantics: divide-by-zero returns -1 (DIV) /
  the dividend (REM); signed `INT_MIN / -1` returns `INT_MIN`.
- Shift amounts must be 0..31 when given as a literal.
- Single primaries still work: `let r = 7`, `let r = a0`.

#### Increment / decrement shorthand

```
let <reg>++             ; rd = rd + 1
let <reg>--              ; rd = rd - 1
```

Both compile to a single `addi rd, rd, ±1` and skip the `=` syntax. They work
with aliases (`name counter = s0; let counter++`) and are rejected on `x0` /
`zero` (writes there are silently dropped in RISC-V — disallowed here to
catch typos).

Useful in counted loops:

```
let i = 0
while i < 10
    ; ... body ...
    let i++
endwhile
```

### name

Introduces a new identifier as an alias for an existing register. The alias
can be used anywhere a register name is expected, including in later `name`
declarations.

```
name <ident> = <reg>
```

Examples:

```
name counter = x5
let counter = 10

name flags = a3
let flags = 0x1000
```

Restrictions:

- the identifier cannot shadow a built-in register name
- redeclaring an existing alias is an error
- the keywords `let` and `name` cannot be used as identifiers

### if / elseif / else / endif

Conditional branching. Blocks are bounded by keywords; nesting is allowed.

```
if <cond>
    ...
elseif <cond>
    ...
else
    ...
endif
```

A `<cond>` is one of:

- a single register — true if the value is non-zero
- a comparison `<primary> <op> <primary>` with `op` one of `< <= > >= == !=`
- a literal (or a literal-only comparison) — folded at compile time; one branch
  becomes unconditional and the unreachable instructions are still emitted but
  never executed.

Examples:

```
if a0 < 0
    let a1 = -1
elseif a0 == 0
    let a1 = 0
else
    let a1 = 1
endif

if a0                   ; truthy: enters if a0 != 0
    let a1 = a1 + 1
endif

if t0 == 5               ; reg vs small literal: uses t6 as scratch (see below)
    let a2 = 42
endif
```

#### Codegen notes

- Two-register comparisons lower to one branch (`bge / blt / beq / bne`),
  picking the negated predicate so the branch fires when the condition is false.
- `if reg <op> 0` is free — the parser promotes literal `0` to `x0`, so it stays
  a one-instruction branch.
- For `if reg <op> <non-zero literal>`, the compiler emits a one-instruction
  setup (`slti` or `xori`) into `t6` (`x31`), then a branch on `t6`.
  **Reserved scratch:** the assembler may clobber `t6` whenever an `if`
  condition compares a register against a non-zero literal — don't keep live
  values in `t6` across such an `if`.
- `if literal <op> reg` is rejected for ordering ops (no `subi` / `slti` with
  the immediate on the left); swap the operands.
- Branch ranges are 13-bit signed (±4 KiB) for conditionals and 21-bit signed
  (±1 MiB) for the implicit unconditional jumps that skip later `elseif`/`else`
  blocks. The compiler reports an out-of-range error if a block is too large.

### while / endwhile

A standard pre-tested loop. The condition uses the same form as `if`.

```
while <cond>
    ...
endwhile
```

The body executes as long as `<cond>` is true. Empty body is allowed (`while 0 endwhile`).

Examples:

```
; sum 1..10 into a0
let a0 = 0
let a1 = 1
while a1 <= 10
    let a0 = a0 + a1
    let a1 = a1 + 1
endwhile

; countdown
let a2 = 5
while a2
    let a2 = a2 - 1
endwhile

; infinite loop (until you hit Stop, or hit an ebreak/illegal inst)
while 1
    let a3 = a3 + 1
endwhile

; same thing, no condition required
while
    let a3 = a3 + 1
endwhile
```

`while` with no condition is shorthand for `while 1`.

#### Codegen

- Lowers to a header branch-to-false to the loop exit, the body, then an
  unconditional `jal x0, header` to repeat.
- `while 1` (and bare `while`) skips the header branch entirely — pure loop with a single back-edge.
- `while 0` emits an unconditional jump over the body (the body is still
  assembled but unreachable).
- The backward jump is a J-type with 21-bit signed range (±1 MiB).

`if`/`while` blocks may be nested in any combination, but `endif` only closes
an `if` and `endwhile` only closes a `while`. Crossed nesting is reported as
"closes the wrong block".

#### break / continue

Available inside a `while` body (including inside an `if` nested in the loop).
Each targets the **innermost enclosing `while`**.

```
break        ; exit the loop immediately
continue     ; jump back to the top of the loop and re-test the condition
```

`break` and `continue` are errors outside any `while`. Both lower to a single
`jal x0, off` — `break` is patched at `endwhile`, `continue` jumps directly to
the loop header (range ±1 MiB).

### getio

Reads a single GPIO pin's current state into a register.

```
getio <pin>, <reg>       ; reg = (GPIO_OUT >> pin) & 1   (0 or 1)
```

- `<pin>` — literal 0..31.
- `<reg>` — destination register (any except `x0`).

Codegen: 2 or 3 instructions depending on the pin number:

| Pin | Sequence | Inst |
|-----|----------|------|
| 0 | `lw reg, -8(x31)` + `andi reg, reg, 1` | 2 |
| 1..31 | `lw reg, -8(x31)` + `srli reg, reg, pin` + `andi reg, reg, 1` | 3 |

`getio` reads `GPIO_OUT` (the chip's drive register, `0xD0000010`) via the
already-loaded `x31` (= `GPIO_OUT_SET = 0xD0000018`). Both `setio` and `getio`
share the same one-instruction GPIO prelude (`lui x30, 0xD0000` + offsets) and
reserve `x30` / `x31`.

### shiftio

Clock one bit of a GPIO pin into a register, MSB-first.

```
shiftio <pin>, <reg>     ; reg = (reg << 1) | ((GPIO_OUT >> pin) & 1)
```

- `<pin>` — literal 0..31.
- `<reg>` — destination register (any except `x0` and `t4` — the assembler uses
  `t4` to collect the bit, so the destination has to be a different register).

Useful for bit-banged serial inputs (SPI MISO, manual UART RX sampling, etc.):
shift one bit per `shiftio`, then act on the accumulated value once you have a
full byte/word.

Codegen — 4 instructions for pin 0, 5 for 1..31:

| Pin | Sequence | Inst |
|-----|----------|------|
| 0 | `lw t4,-8(x31)` + `andi t4,t4,1` + `slli reg,reg,1` + `or reg,reg,t4` | 4 |
| 1..31 | `lw t4,-8(x31)` + `srli t4,t4,pin` + `andi t4,t4,1` + `slli reg,reg,1` + `or reg,reg,t4` | 5 |

Example — clock 8 bits in via pin 0:

```
let acc = 0
let i = 0
while i < 8
    shiftio 0, acc
    let i++
endwhile
; acc now holds the 8-bit MSB-first value sampled across 8 reads
```

### setio

Drives an RP2350-style GPIO output pin via the SIO `GPIO_OUT_SET` /
`GPIO_OUT_CLR` registers (mirrored at `0xD0000018` / `0xD0000020`).

```
setio <pin>, <state>
```

- `<pin>` — literal 0..31 (registers not supported in this round).
- `<state>` — literal; `0` clears the pin, anything else sets it.

The compiler emits the smallest reasonable sequence:

| Pin range | Sequence | Instructions |
|-----------|----------|--------------|
| 0..10 | `addi t4, x0, mask` + `sw t4, 0(addr)` | 2 |
| 11 | `lui t4, 1` + `addi t4, t4, -2048` + `sw` | 3 |
| 12..30 | `lui t4, 1<<(pin-12)` + `sw t4, 0(addr)` | 2 |
| 31 | `lui t4, 0x80000` + `sw t4, 0(addr)` | 2 |

When `state` selects "set", the store goes to `x31`; when it selects "clear",
to `x30`.

#### Reserved registers

If your program uses `setio`, the compiler prepends a one-time three-instruction
prelude that loads:

- `x30` (`t5`) — `0xD0000020` (`GPIO_OUT_CLR` address)
- `x31` (`t6`) — `0xD0000018` (`GPIO_OUT_SET` address)

So `t5`, `t6`, and `t4` (the assembler scratch — also used by `if`/`while`
conditions with an immediate operand) should be considered off-limits in
programs that use `setio`. Programs that don't use `setio` get no prelude
and can use those registers freely.

### setiodir

Configures a GPIO pin's direction (output enable) via the SIO `GPIO_OE_SET` /
`GPIO_OE_CLR` registers (`0xD0000038` / `0xD0000040`). Pins reset as inputs;
call `setiodir <pin>, 1` once at the top of your program for each pin you
intend to drive with `setio`.

```
setiodir <pin>, <state>
```

- `<pin>` — literal 0..31.
- `<state>` — literal; `0` switches the pin to input, anything else to output.

Codegen reuses the `setio` prelude — the `GPIO_OE_*` registers sit at a fixed
`+0x20` offset from the cached `GPIO_OUT_*` addresses, so each `setiodir`
compiles to the same 2- or 3-instruction shape as `setio` (mask materialisation
plus a single `sw` scratch, `0x20(addr)`). No additional registers are reserved
beyond what `setio` already claims. Using `setiodir` implicitly triggers the
GPIO prelude even if your program has no `setio`.

### Packet FIFO commands

Build packets one byte at a time and commit them to the **OUT** packet FIFO
(accessible via the **Packet FIFOs** panel). Used for scripting traffic
out of the simulated chip.

```
pktstartout               ; open a new packet slot (start time auto-captured)
pktadd <reg>               ; append the low byte of <reg> to the open packet
pktdoneout                 ; publish the packet to the FIFO (end-delta auto-recorded)
```

Packets can be up to **64 bytes**; bytes beyond that are dropped and counted in
the FIFO's `droppedOversize` counter. The OUT FIFO holds **32 packet slots**;
if `pktdoneout` runs while the FIFO is full, the packet is discarded and the
`droppedFull` counter is bumped instead of publishing.

Each published packet carries two timestamp fields, captured by hardware (the
simulator captures them at the cycle of the `sw` instruction):

- `m_iStartTime` — 64-bit absolute cycle count, latched on `pktstartout`.
- `m_iEndTime` — 32-bit delta `(time - m_iStartTime)`, recorded on `pktdoneout`.

The 32-bit delta overflows after ~28.6 s at 150 MHz; packets longer than that
will see a wrapped value.

Example:

```
pktstartout
let a0 = 0xDE
pktadd a0
let a0 = 0xAD
pktadd a0
pktdoneout
```

#### Codegen

- Each command compiles to a single `sw` instruction targeting a fixed offset
  of the OUT FIFO base.
- When any packet command appears in a program, the compiler prepends a
  one-instruction prelude (`lui x28, 0xE0000`) that loads the FIFO base into
  **`x28`** (**`t3`**). `t3` is reserved while any pkt command is used.

| Command | Instruction emitted |
|---------|---------------------|
| `pktstartout` | `sw x0, 0(x28)` |
| `pktadd <reg>` | `sw reg, 4(x28)` |
| `pktdoneout` | `sw x0, 8(x28)` |
| `pktstartin <reg>` | `lw reg, 32(x28)` |
| `pktread <reg>` | `lw reg, 36(x28)` |
| `pktdonein` | `sw x0, 40(x28)` |

#### IN FIFO usage

Inbound packets arrive in the **IN FIFO** (queued from outside via the panel
or programmatically via `sim.inFifo`). Reading drains them one packet at a
time:

```
pktstartin <reg>          ; reg = next packet's byte count, 0 if packet empty
                          ; resets the read cursor to byte 0
pktread <reg>              ; reg = next byte; returns 0 once cursor is past end
pktdonein                  ; pop the front packet
```

Typical loop:

```
while 1
    pktstartin t0           ; t0 = packet size or 0
    if t0 == 0
        break
    endif
    let s1 = 0
    while s1 < t0
        pktread a0           ; a0 = next byte
        ; ...process a0...
        let s1 = s1 + 1
    endwhile
    pktdonein                ; pop and move to next packet
endwhile
```

In the **Packet FIFOs** panel the IN section lists queued packets; the head
packet is marked `<- reading` and its bytes show a `|` cursor at the next
position `pktread` will return. Use the `hex bytes` input to inject test
packets without writing C++.

### wait

Stall execution for a precise duration, expressed in cycles or in real time.

```
wait <number>              ; default unit = cycles
wait <number> cycles
wait <number> ns
wait <number> us
wait <number> ms
wait <number> s
```

Real-time units are converted to cycles at compile time using the current
clock rate (set in the Controls panel; default 150 MHz). For example at
150 MHz, `wait 10 us` becomes `wait 1500` (cycles).

#### Codegen

The compiler picks the smallest sequence whose total cycle count exactly
matches the requested duration:

| Cycles | Sequence |
|--------|----------|
| 0 | (nothing) |
| 1..5 | `addi x0, x0, 0` repeated N times (a no-op = 1 cycle) |
| 6+ | `li t4, count` (1?2 inst) + `addi t4, t4, -1` + `bne t4, x0, loop` (2 cycles per iteration) + 0..3 trailing nops to nail parity |

The loop counter uses **`t4`** (`x29`) — the same scratch register the if/while
condition lowering uses. Don't keep live values in `t4` across a `wait`.

`wait 0` emits nothing. `wait` of a negative literal is rejected at compile
time.

`time` and `timeh` are built-in pseudo-primaries that read the system timer.
The simulator backs the timer with the retired-instruction count, so it ticks
once per executed instruction and is fully deterministic — two runs of the
same program produce identical timestamps. The counter resets to 0 on `Reset`.

```
let a0 = time              ; low 32 bits  -> rdtime  a0
let a1 = timeh              ; high 32 bits -> rdtimeh a1
```

These compile to `csrrs rd, 0xC01, x0` (time) and `csrrs rd, 0xC81, x0`
(timeh).

To capture the full 64-bit value safely (in case a carry happens between
reads), use the standard double-read pattern: read `timeh`, then `time`, then
`timeh` again — if the two `timeh` reads match, the pair is consistent.

Limitations:

- Both must stand alone on the RHS — `let a0 = time + 5` is not supported;
  read into a register first, then operate.

#### Inside a task

When `wait` appears inside a `taskstart`/`taskend` block it becomes a
**deadline yield**: the task surrenders control to the scheduler and is
not re-entered until at least `N` cycles have elapsed. Other tasks run in
the meantime. See taskstart / taskend / yield for the full scheduler description.

### GPIO playback file

A separate text file (loaded from the **GPIO Playback** panel) that drives
the simulated GPIO pins against simulator time. This is *not* compiled to RV32I —
it's interpreted by the GUI and applied to the simulator's `gpioOut` register
through the same `GPIO_OUT_SET` / `GPIO_OUT_CLR` aliases that `setio` uses, so
each playback event also shows up in the Logic Analyzer trace.

Format — one instruction per line, `;` or `#` begin comments:

```
setio <pin>, <state>       ; state 0 = low; anything else = high
wait <number><unit>         ; unit = ns | us | ms | s
```

Wait durations are converted to cycles using the current clock rate; if you
change the clock after loading, the schedule is automatically re-derived from
the source. Reset and Compile rewind the playback to the first event so it
restarts in lock-step with the simulator.

Example:

```
setio 0, 1
wait 100 ns
setio 0, 0
wait 1 us
setio 5, 1
```

### stop

Explicit halt. Compiles to a single `jal x0, 0` (jump-to-self), parking the PC
forever. Click Stop in the Controls panel — or hit a breakpoint somewhere
earlier — to break out.

```
stop
```

This is the same instruction the compiler appends after your code; using `stop`
just lets you place one in the middle of a program (typically to end a
top-level event loop's idle branch).

### taskstart / taskend / yield

Defines a cooperative task. Statements before the first `taskstart` are init
code that runs once at boot; everything else lives inside a `taskstart`/
`taskend` block. After the first `taskend`, only further `taskstart` blocks
may appear at top level.

```
; init code runs once, in source order
setiodir 0, 1

taskstart blinker
    setio 0, 1
    wait 1 ms
    setio 0, 0
    wait 1 ms
taskend

taskstart counter
    let a0 = a0 + 1
    yield
taskend
```

- `yield` cooperatively hands control to the next ready task and returns when
  the scheduler picks this task again.
- `wait <N>` inside a task is a **deadline yield**: control hands off until
  at least `N` cycles have elapsed before this task can resume. Outside a
  task `wait` remains a busy-loop (existing behavior).
- Tasks cannot nest. Falling off `taskend` implicitly loops back to the
  task's first instruction (tasks are infinite event loops by default).
- `break` / `continue` target the innermost `while`, not the task. `stop`
  halts the entire CPU.
- Maximum **8** tasks per program. A program with zero `taskstart` blocks
  compiles bit-identically to today — the scheduler is opt-in.
- Deadline limit: any single `wait` literal must fit in `2^31 - 1` cycles
  (~14 s at 150 MHz). For longer sleeps, loop multiple shorter waits.

#### Reserved registers

When at least one `taskstart` is present, the compiler reserves **`s11`**
(x27) as a dispatcher scratch in addition to the already-reserved `t4`.
Don't keep live values in `t4` or `s11` across a `yield` or a `wait` inside
a task. (`t3` / `t5` / `t6` reservations from `pkt*` / `setio` / `getio` still
apply when those features are used; tasks do not conflict.)

#### Codegen

- Memory layout: the binary starts with `jal x0, init_start` (1 instruction),
  followed by an 8-byte slot per task at PC `4..4 + 8N - 1`. Each slot holds
  `{ resume_PC, wake_time }` (initialized at compile time so reset restores
  the original entry state automatically).
- Every `yield` site inlines a self-contained dispatcher: 3 instructions to
  save the resume PC, then `4N + 2` instructions to probe each task slot in
  round-robin order (starting at `(i+1) mod N`) and jump to the first
  ready one. Total: `4N + 5` instructions per `yield` site.
- Every in-task `wait <N>` adds 3 more instructions (read `time`, add
  `N`, store as wake), for `4N + 8` total (or `4N + 10` when `N > 2047`).
- The dispatcher loops back to the top of its probe chain when all tasks
  are blocked, refreshing `time` each pass — eventually some deadline
  elapses and the corresponding task runs.

### End-of-program halt

The compiler appends a `jal x0, 0` (jump-to-self) instruction after your code.
When execution falls off the end of your program, the PC parks on that
instruction instead of stepping into the zero-filled RAM beyond it. The Run
button keeps stepping over the halt forever; click Stop to break out.

The result struct exposes `userCodeSize` (bytes of source-generated code) as
the boundary, so callers that want to halt automatically can stop when
`pc >= userCodeSize`.

### Diagnostics

Each error reports a line and column. The compiler recovers at the next
newline, so a single compile run reports as many errors as it can find.
