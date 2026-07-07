# psk — software BPSK modulator (`mod_bpsk`)

`kernel/psk.c`, tested by `tests/test_psk.c` / `tests/gen_vectors.py`.

Maps a stream of bits (one 32-bit float per bit, each exactly `0.0f` or
`1.0f`) to amplitude-scaled BPSK symbols, entirely in the VCPU's general
vector arithmetic pipeline — no hardware constellation-mapping accelerator
involved. See `psk_hw/doc/psk_hw.md` for the accelerator-based alternative.

## The mapping

BPSK is just `symbol = 2*bit - 1`, scaled by a headroom amplitude to leave
margin for downstream pulse-shaping-filter overshoot:

```
symbol = 2 * TX_AMPLITUDE * bit - TX_AMPLITUDE      (TX_AMPLITUDE = 0.9)
```

This is an affine, branchless formula — no per-bit conditional, which
matters because vector lanes can't take per-element branches. It maps
directly onto one hardware instruction: `rmad` (real multiply-add),
`V[i] = S0[i]*S1[i] + S2[i]`, with `S0 = 2*TX_AMPLITUDE` (broadcast),
`S1 = bit`, `S2 = -TX_AMPLITUDE` (broadcast).

## Pipeline setup

```c
__set_prec(single, single, single, single, half_fixed);
__set_Smode(S0hword, S1straight, S2straight);
```

- `S0prec/S1prec/S2prec = single`: all three inputs are read as plain IEEE
  floats (`bit_in` is `0.0f`/`1.0f`, `factor`/`clamp` are ordinary C floats).
- `Vprec = half_fixed`: the *output* is written in VSPA's half-fixed
  sign-magnitude format (1-bit sign + 15-bit fraction — **not** two's
  complement), matching the fixed-point convention the rest of the TX chain
  expects.
- `S0hword`: broadcast the single scalar `factor` to all 64 lanes uniformly.
- `S1straight`: stream the actual per-bit vector data, unmodified.
- `S2straight`: stream the actual per-lane `clamp` vector, unmodified (not
  `S2zeros` — this kernel needs a real per-lane offset, not a hardwired
  constant).

## VRA pointer layout

- `rV`/`rSt` both point at row 5 (`_VR5`) — the row `wr.straight` writes
  computed output into, and the row `st.laddr`(`__st_vec`) reads back out to
  push to DMEM. Arbitrary row choice; just needs to not collide with the
  rows used for the read-once scalar constants below.
- `factor` is loaded into row 0 (`__ld_Rx_mem_unaligned(0, ...)`), then `rS0`
  is pointed at row 0 and read *once*, before the loop — its value doesn't
  change across bits, so it's latched once and reused every iteration
  exactly like `mod_bpsk_hw`'s normalization constant.
- `clamp` (a full 32-element array, one `-TX_AMPLITUDE` per lane) is loaded
  into row 2, `rS2` reads it once, same reasoning.
- `bit_in` is reloaded into row 1 **every iteration** (`rS1`/`rd_S1` inside
  the loop) — this is the one operand that genuinely changes each pass.

## Loop

One iteration processes one full `VRA_WORD_CAPACITY` (32) chunk of bits:

```c
for (i = 0; i < N/32; i++) {
    __ld_Rx_mem_unaligned(1, bit_in + i*32);
    __set_VRAptr_rS1(_VR1);
    __rd_S1();
    __rmad();
    __wr(straight);
    __st_vec((vspa_vector_pair_fixed16 *) bpsk_out + i);
}
```

## Preconditions

- **`N` must be a multiple of 32.** Non-multiple-of-32 `N` is a documented
  caller precondition (pad the input), not handled inside the kernel — this
  was a deliberate scope decision, not an oversight.
- Output is real-only: `bpsk_out`'s imaginary component is whatever was
  already in that VRA row before `wr.straight` (irrelevant here since
  `rmad`+`wr.straight` is a pure real-mode pipeline; the "complex" output
  type is just how the destination buffer's memory happens to be typed/
  reused downstream).

## What this kernel does *not* do

Amplitude clipping here only provides headroom against a *later*
pulse-shaping filter's overshoot — RRC filtering, mixing, and the rest of
the RF TX chain are out of scope for this kernel and not implemented
anywhere in this test project.
