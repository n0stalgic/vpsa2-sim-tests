# psk - software BPSK modulator (`mod_bpsk`)

`kernel/psk.c`, tested by `tests/test_psk.c` / `tests/gen_vectors.py`.

Maps bits to amplitude-scaled BPSK symbols using the VCPU's general vector
pipeline, no `ld.qam` accelerator (see `psk_hw/doc/psk_hw.md` for that
version). Input is one 32-bit float per bit, exactly `0.0f` or `1.0f`.

## The mapping

```
symbol = 2 * TX_AMPLITUDE * bit - TX_AMPLITUDE      (TX_AMPLITUDE = 0.9)
```

Affine and branchless on purpose, vector lanes can't take per-element
branches. Maps directly to `rmad`: `V[i] = S0[i]*S1[i] + S2[i]`, with
`S0 = 2*TX_AMPLITUDE` (broadcast), `S1 = bit`, `S2 = -TX_AMPLITUDE`
(broadcast).

## Pipeline setup

```c
__set_prec(single, single, single, single, half_fixed);
__set_Smode(S0hword, S1straight, S2straight);
```

Inputs are read as plain floats; output is `half_fixed` (VSPA's own
sign-magnitude fixed-point, not two's complement). `S0hword` broadcasts
`factor` to all lanes, `S1straight`/`S2straight` stream the real per-lane
`bit`/`clamp` data through unchanged (`S2straight`, not `S2zeros`, since
`clamp` is a real per-lane value here, not a hardwired constant).

## VRA layout

`rV`/`rSt` both point at row 5, the `wr.straight` output row that
`__st_vec` reads back for DMEM. `factor` (row 0) and `clamp` (row 2) are
each loaded and read once before the loop, since neither changes across
bits. `bit_in` is reloaded into row 1 and re-read every iteration, since
that's the operand that actually changes.

## Loop

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

## Preconditions and scope

`N` must be a multiple of 32, a caller precondition (pad the input), not
handled internally. This was a deliberate scope choice.

Output is real-only; the imaginary half of `bpsk_out` is just leftover VRA
content, since `rmad`+`wr.straight` is a pure real-mode pipeline here.

Amplitude clipping is only about leaving headroom for a later
pulse-shaping filter. RRC filtering, mixing, and the rest of the RF TX
chain aren't implemented anywhere in this test project.
