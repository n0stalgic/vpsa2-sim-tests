# psk_hw - `ld.qam` hardware-accelerated modulators

`kernel/psk_hw.c` (`mod_bpsk_hw`, `mod_qpsk_hw`), tested by
`tests/test_psk_hw.c` / `tests/gen_vectors.py`. See `psk/doc/psk.md` for
the software version.

## Where `ld.qam` sits

`ld.qam Rx` is a load instruction, on the same DMEM read bus as
`ld.normal` (ISM Table 33). Constellation mapping happens while data is
loading, before the S0/S1/S2 + `rmad` pipeline ever sees it. It's
dedicated table-lookup hardware on the load path, not part of the AU.

```c
__ld_vec(bit_in + i);   // issues the DMEM read address
__ld_Rx(qam, 2);        // transforms the loaded data into 32 complex
                         // symbols, written to R2
```

`__ld_Rx(mode, n)` takes no address, it operates on data already in
flight from a preceding `__ld_vec`. For a mode with `M` bits/symbol, one
`__ld_vec` + `__ld_Rx(qam,n)` pair pulls `M` bits per symbol and produces
a full line (32 symbols) of half_fixed complex output.

## Register configuration

```c
iowr(LD_RF_CONTROL, <mode>);           // 1=BPSK, 2=QPSK, 4=16QAM, 6=64QAM (nibble), 8=256QAM, 10=1024QAM
iowr(LD_RF_TB_REAL_0, <real coeffs>);  // only some modes need this, see below
iowr(LD_RF_TB_IMAG_0, <imag coeffs>);
```

### Why BPSK/QPSK write coefficients but 16-QAM doesn't

A note on `LD_RF_CONTROL`'s mode field:

> Writing anything other than 6, 8 or 10 to the mode bits will
> automatically update the `LD_RF_TB_REAL_0` and `LD_RF_TB_IMAG_0`
> registers to 16QAM values. Writing 6 will automatically update these
> registers to 64QAM values.

Setting mode to 1, 2, or 4 always auto-loads the 16-QAM default
coefficients as a side effect. BPSK/QPSK overwrite them right after
because those defaults are wrong for their smaller alphabets. 16-QAM
doesn't, because the auto-loaded defaults are already correct for it.

### The coefficient values

Two independent layers. First, for `M <= 4`, each coefficient slot is a
2-bit code selecting one of 4 fixed values, hardwired: `00`=+1, `01`=+3,
`10`=-1, `11`=-3. Second, which code goes in which slot is what you
configure: `ld.qam`'s combined index `j` (all `M` bits together) picks a
slot, index `i` at bits `(2i+1):(2i)`. A mode needs `2^M` slots: 2 for
BPSK, 4 for QPSK, 16 for 16-QAM (exactly fills one register at 2
bits/slot, hence the datasheet labeling that table "Mode==0100"). 64-QAM
needs 64 slots, too many for this scheme, so it switches to nibbles across
8 registers instead.

Mental model: forget phase and PSK, this hardware has no concept of
angle. It's two independent PAM engines, one per axis, each picking one of
up to 4 voltage levels from an input code. `LD_RF_TB_REAL_0` is the
I-engine's table, `LD_RF_TB_IMAG_0` is the Q-engine's. Both see the same
combined index `j` (a wiring detail), each is programmed to ignore
whatever bits aren't its own.

For BPSK/QPSK, one bit per axis, no Gray-coding order to worry about:
I-engine reads bit0, Q-engine reads bit1, both apply bit=0 -> -1, bit=1 -> +1.

| `j` | bit0 (I) | bit1 (Q) | REAL | IMAG |
|---|---|---|---|---|
| 0 | 0 | 0 | -1 | -1 |
| 1 | 1 | 0 | +1 | -1 |
| 2 | 0 | 1 | -1 | +1 |
| 3 | 1 | 1 | +1 | +1 |

Packing these gives `qam.sx`'s actual values: `LD_RF_TB_REAL_0 =
0x00000002` for BPSK, and `0xffffff22`/`0xffffff0a` for QPSK's real/imag
(upper 24 bits are unused padding, `M=2` never lets `j` exceed 3).

16-QAM has 2 bits/axis (4 levels, not 2), so bit-to-level order actually
matters there. Don't assume the BPSK/QPSK pattern generalizes, check
`model.py`'s `map_table['16qam']` instead of deriving it by hand.

Datasheet typo to know about: one sentence calls these registers
`LD_RF_REAL0`/`LD_RF_IMAG0` (no `_TB_`). Everywhere else, including the
registers' own section headers, it's `LD_RF_TB_REAL_0`/`LD_RF_TB_IMAG_0`.

## Normalization, the `x8`

Every mode loads a normalization constant (`QAM_NORMALIZATION_*` in
`qam.h`) and multiplies it by 8 first:

```c
#define QAM_NORMALIZATION_BPSK  0x41000000   /* 1.0 * 8, raw f32 bits */
#define QAM_NORMALIZATION_QPSK  0x40B504F3   /* (1/sqrt(2)) * 8 */
```

Confirmed necessary empirically (BPSK is off by exactly 8x without it).
Partially explained: `set.creg 255, 0` disables `HPFS`/`HPFV`, a hardware
auto-doubling of half_fixed data, accounting for one factor of 2. Where
the other factor of 4 comes from isn't documented anywhere we found.
Treat the constant as verified correct, not fully derived.

These are raw `uint32_t` bit patterns, not float literals. `__mv_w_rV`
accepts either and moves the bits verbatim into VRA, same as `qam.sx`'s
own `mv g3, QAM_NORMALIZATION_BPSK`. Writing it this way (`mv.w [rV]`)
skips DMEM entirely, unlike `mod_bpsk`'s constants which load from memory.

## VRA layout

Same row-reuse trick both kernels share, rows just swapped:

- `rV`/`rS0` share a scratch row (`_VR0`). `mv_w_rV` writes the norm
  constant there, `rd_S0` latches it once before the loop.
- `rS1` points at whatever row `__ld_Rx(qam, n)` writes (`_VR2`), re-read
  every iteration since that data changes each pass.
- `rSt` matches `rV`'s row. Once `rd_S0` has already read the constant
  out, it's safe for the loop to overwrite that row with real output.

`rV`, `wr.straight`, and `mv.w [rV]` are all tied specifically to the `rV`
pointer, no equivalent exists for `rS0`/`rS1`/`rS2`/`rSt`. Which row `rV`
points at is otherwise free, just keep `rS0`/`rSt` matching and avoid
`ld.qam`'s destination row.

## Smode differs between BPSK and QPSK, unresolved

`mod_bpsk_hw` uses `S0i1r1i1r1` (complex pair, interleaved broadcast).
`mod_qpsk_hw` uses `S0hword` (plain real scalar, uniform broadcast). Both
match `qam.sx`, for what looks like the same job.

Guess, not confirmed: only a plain real word ever gets written via
`mv.w [rV]` (never the complex form), so `S0i1r1i1r1`'s "imaginary" slot
may read uninitialized VRA content. Harmless for BPSK (its S1 imaginary
part is always 0, garbage times 0 is still 0), but QPSK's S1 imaginary
part is real, so garbage there could actually break it, which would
explain the switch to `S0hword`. If adding a mode with complex output,
default to `S0hword` rather than assuming `S0i1r1i1r1` generalizes.
