# psk_hw — `ld.qam` hardware-accelerated modulators

`kernel/psk_hw.c` (`mod_bpsk_hw`, `mod_qpsk_hw`), tested by
`tests/test_psk_hw.c` / `tests/gen_vectors.py` (mode selected via
`QAM_MODE` env var / `-DQAM_MODE`). See `psk/doc/psk.md` for the
pure-software alternative this replaces the constellation-mapping half of.

## Where `ld.qam` sits in the pipeline

`ld.qam Rx` is a **load-class instruction** — it lives on the same
`memRead`/DMEM bus as `ld.normal`, `ld.h2l`, etc. (ISM Table 33, "Load VRA
Modes"). Constellation mapping happens *while loading*, before the general
S0/S1/S2 + `rmad` arithmetic pipeline ever sees the data — it is dedicated
table-lookup silicon bolted onto the load path, not part of the AU.

C-level, this is two separate steps, not one:
```c
__ld_vec(bit_in + i);   // ld [aX]+0 -- issues the DMEM read address
__ld_Rx(qam, 2);        // ld.qam R2 -- transforms whatever landed on the
                         //              bus into 32 complex symbols in R2
```
`__ld_Rx(mode, n)` carries no address itself — it operates on data already
in flight from a preceding `__ld_vec`. This mirrors the SDK's own
`__ld_Rx_mem(n, addr) = { __ld_vec(addr); __ld_normal_Rx(n); }` convenience
macro, just with `qam` in place of `normal`.

Per modulation order `M` (bits/symbol), `ld.qam` pulls `M` bits per output
symbol from the bus, maps each through the constellation LUT, and writes a
**half_fixed** complex pair per symbol into the destination row — one
`__ld_vec`+`__ld_Rx(qam,n)` pair processes one full line (32 symbols).

## Two control planes, two lifetimes

1. **IP-bus registers** (`iowr`/`iord`, defined in `iohw.h`) — `LD_RF_CONTROL`
   is explicitly documented as a "**Slow read register**": one-time
   peripheral-style configuration, not part of the per-cycle VLIW datapath.
2. **VRA/AU pipeline state** (`set.creg`/`set.prec`/`set.Smode`, the
   `rV`/`rS0`/`rS1`/`rSt` pointers) — also configured once, but this is the
   fast data-plane machinery that runs every cycle for the rest of the
   kernel.

## Register configuration

```c
iowr(LD_RF_CONTROL, <mode>);           // 1=BPSK, 2=QPSK, 4=16QAM, 6=64QAM(nibble), 8=256QAM, 10=1024QAM
iowr(LD_RF_TB_REAL_0, <real coeffs>);  // only for modes that need non-default coefficients -- see below
iowr(LD_RF_TB_IMAG_0, <imag coeffs>);
```

### The `tblWriteEn_b` auto-update mechanism — why BPSK/QPSK need explicit
### coefficient writes but 16-QAM doesn't

Straight from the ISM's `LD_RF_CONTROL` field description (easy to miss —
it's a note attached to the `mode` field, not its own register):

> Writing anything other than 6, 8 or 10 to the mode bits will
> **automatically update** the `LD_RF_TB_REAL_0` and `LD_RF_TB_IMAG_0`
> registers **to 16QAM values**. Writing 6 will automatically update these
> registers to 64QAM values.

So setting `LD_RF_CONTROL` to *any* of `{1,2,4}` always auto-loads the
16-QAM default coefficients as a side effect — regardless of which of those
three modes you actually asked for. That's exactly why:

- `_qamModBpsk`/`_qamModQpsk` **both** write `LD_RF_CONTROL` *and then
  immediately overwrite* `LD_RF_TB_REAL_0`/`IMAG_0` — the auto-loaded
  16-QAM defaults are wrong for their smaller (`±1`-only) alphabets, so the
  explicit write is there to *correct* the auto-update, not to configure
  something that would otherwise be unset.
- `_qamMod16` writes **only** `LD_RF_CONTROL=4` and touches nothing else —
  the auto-loaded defaults *are* exactly the 16-QAM coefficients it needs,
  so there's nothing to correct.

**Implication for adding a new mode:** if the mode's natural coefficient
table matches what auto-loads for its mode class, you may not need to write
`LD_RF_TB_*` explicitly at all — worth testing empirically (build without
the explicit write, see if it still matches the oracle) before assuming
you need to derive and write coefficients by hand.

### Decoding/constructing `LD_RF_TB_REAL_0`/`IMAG_0` values

Two independent layers:

**Layer 1 — fixed in silicon, not configurable.** For `mode<=0100`, each
coefficient slot is a 2-bit *code* selecting one of exactly 4 fixed output
values: `00→+1, 01→+3, 10→-1, 11→-3`. This table is hardwired; no register
changes what code `10` means.

**Layer 2 — which code goes in which slot is the actual configuration.**
`ld.qam`'s combined index `j` (all `M` bits of the symbol, not split into
per-axis sub-indices) selects a slot per the rule "index `i` occupies bits
`(2i+1):(2i)`". A mode needs `2^M` slots — 2 for BPSK (`M=1`), 4 for QPSK
(`M=2`), 16 for 16-QAM (`M=4`, exactly filling one 32-bit register at 2
bits/slot — this is why the datasheet's summary table is headed
"Mode==0100": 16-QAM is the case that exactly saturates this format).
64-QAM (`M=6`) needs `2^6=64` slots, too many for 2-bit codes in one
register — hence it switches to a completely different scheme (8 registers
× 8 nibbles, 3 significant bits + 1 spare per nibble, ISM Table for
`mode==0110`).

**The mental model that actually makes this tractable: forget phase/PSK
entirely.** This hardware has no concept of angle. Think of it as two
independent, non-interacting PAM engines — one drives the real/I axis, one
drives imaginary/Q — each just picking one of up to 4 fixed voltage levels
from an input code. `LD_RF_TB_REAL_0` is the I-engine's lookup table,
`LD_RF_TB_IMAG_0` is the Q-engine's, and even though both engines are shown
the *same* combined index `j` (a hardware wiring quirk, not a geometric
necessity), each is programmed to ignore whichever bits aren't its own.

Concretely, for BPSK/QPSK (single bit per axis, no Gray-coding complexity
since there's only one bit to assign): I-engine reads bit0 of `j`, Q-engine
reads bit1, and each independently applies `bit=0→-1, bit=1→+1`:

| `j` | bit0 (I) | bit1 (Q) | REAL | IMAG |
|---|---|---|---|---|
| 0 | 0 | 0 | -1 | -1 |
| 1 | 1 | 0 | +1 | -1 |
| 2 | 0 | 1 | -1 | +1 |
| 3 | 1 | 1 | +1 | +1 |

Converting each needed value to its Layer-1 code and packing per the
`bits(2i+1):(2i)` rule reproduces the actual values used in `qam.sx`:
- `LD_RF_TB_REAL_0 = 0x00000002` for BPSK (`j` only takes 0/1; codes `10,00`)
- `LD_RF_TB_REAL_0/IMAG_0 = 0xffffff22 / 0xffffff0a` for QPSK (codes
  `10,00,10,00` / `10,10,00,00` for `j=0..3`; upper 24 bits are don't-care —
  unreachable since `M=2` caps `j` at 0-3)

**Watch out for 16-QAM specifically:** with 2 bits per axis (4 levels
`{+1,+3,-1,-3}` instead of 2), the bit→level assignment is no longer a
trivial single-bit flip — Gray-coding order matters once multiple bits
jointly select one axis's level (unlike BPSK/QPSK, where a single bit has
no ordering to get wrong). Don't assume the same "bit=0→low value" pattern
generalizes without checking `model.py`'s actual `map_table['16qam']`
against whatever bit weighting `ld.qam` uses.

One outright **datasheet typo**: one sentence refers to these registers as
"`LD_RF_REAL0`/`LD_RF_IMAG0`" (missing the `_TB_` infix) — that's the only
place in the whole manual it's spelled that way; everywhere else, including
the registers' own section headers, it's `LD_RF_TB_REAL_0`/`LD_RF_TB_IMAG_0`.

## Normalization scaling — the `×8`

Every mode's kernel loads a normalization constant (`QAM_NORMALIZATION_*`
in `qam.h` — `1.0` for BPSK, `1/√2` for QPSK, `1/√10` for 16-QAM, etc.,
i.e. the standard "equalize average symbol energy across modulation
orders" scale) and **pre-multiplies it by 8** before using it:

```c
#define QAM_NORMALIZATION_BPSK  0x41000000   /* 1.0 * 8, as raw f32 bits */
#define QAM_NORMALIZATION_QPSK  0x40B504F3   /* (1/sqrt(2)) * 8 */
```

Confirmed empirically necessary (BPSK output is off by a clean factor of 8
without it) and traced *partially* to the datasheet: `set.creg 255, 0`
(used by every mode) sets `HPFS`/`HPFV` ("Half fixed scale by 2 for
S1"/"for V") to 0, disabling a hardware auto-doubling of half_fixed data —
accounting for one factor of 2. The remaining factor of 4 isn't spelled out
anywhere we found in the ISM; treat the constant as verified-correct
(matches NXP's own shipped `qamModBpsk`/`qamModQpsk`, and the simulator
test is the actual proof) rather than something to re-derive from first
principles.

The constants above are defined as **raw `uint32_t` bit patterns**, not
float literals — `__mv_w_rV` accepts either (per its "Type can be float or
any integer type ≤32 bits" contract), and a raw hex pattern is just the
IEEE-754 bits moved verbatim into VRA, exactly like `qam.sx`'s own
`mv g3, QAM_NORMALIZATION_BPSK` (also a raw hex `#define` in `qam.h`).

### Why `mv.w [rV]` instead of a DMEM round-trip

The norm constant is written directly into VRA via `__mv_w_rV(val)` (asm:
`mv.w [$rV], val`) rather than the DMEM-round-trip pattern `mod_bpsk` uses
for its own constants (`__ld_Rx_mem_unaligned`). `mv.w` moves an
immediate/general-register value straight into VRA — no DMEM bus access,
no load latency. Measured empirically: `mv.w [rV]` → 96 cycles;
switching to `__ld_Rx_mem_unaligned` (4 instructions, 2 DMEM loads, each
carrying the ISM's documented "~3 cycle" memory latency) → 107 cycles.
With full compiler optimization (removing the `#pragma optimization_level
0` safety net — see caveat below) → 40 cycles.

**`#pragma optimization_level 0` is not incidental — every NXP C-kernel
example uses it** (confirmed in both `tone_generator.c` and NXP's own
`mixer_cwproj/main.c`), because the core pipeline intrinsics
(`__set_VRAptr_*`, `__rd_S0`/`__rd_S1`, `__rmad`, `__wr`) are **not**
qualified `volatile` in their `asm` definitions. The compiler has zero
visibility into the hardware-ordering dependencies between them (e.g. that
`__rd_S1()` depends on a preceding `__ld_Rx(qam,...)`), so at higher
optimization it's technically free to reorder or eliminate them. Keep
optimization off for correctness; treat any faster/optimized build as an
experiment to double-check against the generated assembly, not something
to ship.

## VRA pointer layout

Both kernels reuse the same physical-row-reuse trick, just with rows
swapped relative to each other:

- `rV`/`rS0` point at the same scratch row (`_VR0` in the current code) —
  `__mv_w_rV` writes the norm constant there, `rd_S0` reads it back out
  into the S0 pipeline register *once*, before the loop.
- `rS1` points at whichever row `__ld_Rx(qam, n)` targets (`_VR2` here) —
  read fresh via `rd_S1()` every iteration, since that's the operand that
  actually changes each pass.
- `rSt` matches `rV`'s row (as a plain row number, not a `_VRn` alias) —
  once `rd_S0` has already latched the constant out of that row, it's safe
  to let the loop's `wr.straight` start overwriting the same row with real
  per-iteration output, which `st.laddr`(`__st_vec`) then reads back out.

`rV`/`wr.straight`/`mv.w [rV]` are architecturally tied to the `rV` pointer
specifically — there's no equivalent write-scalar-to-VRA or
write-AU-result-to-VRA instruction using `rS0`/`rS1`/`rS2`/`rSt` instead.
*Which row* `rV` points at is freely choosable (just keep `rS0`/`rSt`
pointed at the same row, and don't collide with wherever `ld.qam` writes).

## `Smode` differs between BPSK and QPSK — unresolved

`mod_bpsk_hw` uses `S0i1r1i1r1` (broadcasts one complex `(imag,real)` pair,
duplicated across all lanes in interleaved order) — matching `qam.sx`.
`mod_qpsk_hw` uses `S0hword` (broadcasts one plain real scalar uniformly to
every lane) — also matching `qam.sx`, but a different mode for what looks
like the same job (scaling a complex vector by a real scalar).

Working hypothesis, not fully confirmed: since only a single real word is
ever written via `mv.w [rV], val]` (never the complex form,
`__mv_w_rV_complex(r,i)`), `S0i1r1i1r1`'s "imaginary" broadcast slot may be
reading adjacent, never-explicitly-zeroed VRA content. For BPSK this is
harmless — its `S1` imaginary component is always exactly 0, so
`garbage × 0 = 0` regardless of what's in that slot. For QPSK, `S1`'s
imaginary component is genuinely non-zero, so if that slot really is
uninitialized garbage, `S0i1r1i1r1` could produce wrong output — matching
why `qam.sx` switches to `S0hword` (uniform real broadcast, no separate
"imaginary" companion value to worry about) once the imaginary part
actually matters. Not empirically verified either way; if you're adding a
new complex-output mode, default to `S0hword` (matching QPSK) rather than
assuming `S0i1r1i1r1` generalizes.

## Test harness conventions

- Mode selected via `QAM_MODE` env var (`Makefile`: `export QAM_MODE ?=
  BPSK`), threaded into both `gen_vectors.py` (reads `os.environ`) and the
  C build (`-DQAM_MODE=QAM_MODE_$(QAM_MODE)`), mirroring
  `la931x_vspa_common/vspa-lib/qam/tests/test_qam.c`'s dispatch pattern.
- `gen_vectors.py` reuses the vendored oracle at `psk_hw/python/model.py`
  (copied from NXP's own `qam/python/model.py`) rather than hand-deriving
  each mode's reference math — avoids re-deriving (and re-risking
  transcription errors in) the same constellation math this doc just spent
  a long time reconstructing by hand.
- Always `N_LINES=1` regardless of mode (one packed input line in, 32
  cfloat16 symbols out) — these tests validate the constellation mapping
  itself, not throughput, unlike NXP's own per-mode `N_LINES_PER_MODE`
  table which sizes for realistic throughput.
- `kernel.mk`'s `generate` step is unconditional (always reruns
  `gen_vectors.py` before building) specifically so switching `QAM_MODE`
  without an intervening `make clean` can't silently reuse stale vectors
  from a previous mode.

## Checklist for adding a new mode

1. `LD_RF_CONTROL` value — from `qam.h`'s `QAM_*` constants (`4` for
   16-QAM).
2. Check whether the auto-update default (see above) already gives the
   right `LD_RF_TB_REAL_0`/`IMAG_0` for this mode — if so, skip explicit
   coefficient writes entirely (matches `_qamMod16`'s approach).
3. If not, derive the coefficient register values by hand using the
   two-layer model above (fixed 4-value code table + per-slot assignment
   from the actual bit→level mapping for this mode — check `model.py`'s
   `map_table` for the real assignment, don't assume it's a trivial
   bit-flip once more than 1 bit/axis is involved).
4. Normalization constant from `qam.h`'s `QAM_NORMALIZATION_*`, pre-scaled
   ×8, as a raw `uint32_t` bit pattern.
5. `Smode` — try `S0hword` first for anything with genuinely complex
   output (see unresolved question above); fall back to matching
   `qam.sx`'s actual choice if it doesn't pass.
6. Build with `QAM_MODE=<mode>`, run `make test`, let the simulator (not
   more manual derivation) be the final word on correctness.
