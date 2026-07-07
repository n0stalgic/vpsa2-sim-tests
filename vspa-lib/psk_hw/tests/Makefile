# SPDX-License-Identifier: BSD-3-Clause
# ld.qam hardware-accelerator kernel tests — see ../../common/kernel.mk for
# the shared build/test/clean rules.
#
# Mode is selected via QAM_MODE (BPSK, QPSK, ...), e.g.:
#   make test                  # QAM_MODE=BPSK (default)
#   make QAM_MODE=QPSK test

export QAM_MODE ?= BPSK

KERNEL = qam_hw_$(QAM_MODE)
SRCS   = test_psk_hw.c kernel/psk_hw.c
DEFS   = -DQAM_MODE=QAM_MODE_$(QAM_MODE)

include ../../common/kernel.mk
