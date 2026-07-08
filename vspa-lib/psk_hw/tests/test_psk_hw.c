// SPDX-License-Identifier: BSD-3-Clause
// ld.qam hardware-accelerator kernel test harness -- Python-oracle driven,
// mirroring la931x_vspa_common/vspa-lib/qam/tests/test_qam.c's dispatch
// pattern (mode selected at compile time via -DQAM_MODE=<token>).
//
// Unlike test_qam.c (which sizes N_LINES per mode for throughput testing),
// this always uses N_LINES=1 -- these tests only probe whether ld.qam's
// constellation mapping itself is correct, not throughput. See
// gen_vectors.py for the matching vector generation.
//
// Each output cfloat16 packs as (imag_fp16<<16) | real_fp16 in one uint32;
// compared uint32-wise against ref.hex via vspa_array_cmp.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <vspa/intrinsics.h>
#include "test_utils.h"

// kernel entry points (see kernel/psk_hw.c).
extern void mod_bpsk_hw(unsigned int *bit_in, vspa_complex_float16 *qam_out, unsigned int N);
extern void mod_qpsk_hw(unsigned int *bit_in, vspa_complex_float16 *qam_out, unsigned int N);
extern void mod_16qam_hw(unsigned int *bit_in, vspa_complex_float16 *qam_out, unsigned int N);

// Mode tokens -- mirror QAM_MODE values produced by the Makefile -D.
#define QAM_MODE_BPSK     1
#define QAM_MODE_QPSK     2

#ifndef QAM_MODE
#define QAM_MODE QAM_MODE_BPSK
#endif

#define N_LINES 1
#define N_OUT_SYMBOLS (N_LINES * 32)

// Per-mode geometry: must match gen_vectors.py (N_INPUT_WORDS = N_LINES * M).
#if QAM_MODE == QAM_MODE_BPSK
#  define N_INPUT_WORDS   1
#  define QAM_MOD_FN      mod_bpsk_hw
#elif QAM_MODE == QAM_MODE_QPSK
#  define N_INPUT_WORDS   2
#  define QAM_MOD_FN      mod_qpsk_hw
#elif QAM_MODE == QAM_MODE_QAM16
#  define N_INPUT_WORDS   4
#  define QAM_MOD_FN      mod_16qam_hw
#else
#  error "Unknown QAM_MODE"
#endif

// Compile-time-baked vectors from gen_vectors.py.
static const unsigned int INPUT_DATA[N_INPUT_WORDS] = {
#include "vectors/input.hex"
};

static const unsigned int REF_DATA[N_OUT_SYMBOLS] = {
#include "vectors/ref.hex"
};

_VSPA_VECTOR_ALIGN static unsigned int bitIn[N_INPUT_WORDS];
_VSPA_VECTOR_ALIGN static unsigned int qamOut[N_OUT_SYMBOLS];

int main(void)
{
    int i;

    for (i = 0; i < N_INPUT_WORDS; i++)
        bitIn[i] = INPUT_DATA[i];

    KCYC_INIT();
    KCYC_START();
    QAM_MOD_FN(bitIn, (vspa_complex_float16 *)qamOut, (unsigned int)N_LINES);
    KCYC_STOP_PRINT();

    return vspa_array_cmp(qamOut, REF_DATA, N_OUT_SYMBOLS);
}
