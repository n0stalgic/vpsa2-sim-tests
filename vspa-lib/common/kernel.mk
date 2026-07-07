# SPDX-License-Identifier: BSD-3-Clause
# Shared build/test/clean rules for standalone VSPA kernel tests.
#
# Included from a kernel's tests/Makefile, e.g. vspa-lib/qam/tests/Makefile:
#
#   KERNEL = qam_$(QAM_MODE)
#   SRCS   = test_qam.c ../asm/qam.sx
#   DEFS   = -DQAM_MODE=QAM_MODE_$(QAM_MODE)
#   include ../../common/kernel.mk
#
# KERNEL and SRCS are required. DEFS is optional, kernel-specific -D flags.

MK_DIR      := $(dir $(lastword $(MAKEFILE_LIST)))
VSPA_LIB    := $(abspath $(MK_DIR)/..)
PROJ_ROOT   := $(abspath $(VSPA_LIB)/..)
KERNEL_ROOT := $(abspath ..)

VSPA_TOOL             ?= /opt/VSPA_Tools_vbeta_14_00_781
export SIMULATOR_PATH ?= $(HOME)/code/rf/la9310_chipcode/ccssim2/bin/runsim
AU                    ?= 16
ARCH                   = vspa2
CORE                   = sp
DEFS                  ?=

VSPA_CC      = $(VSPA_TOOL)/bin/fsvspacc
VSPA_SDK_INC = $(PROJ_ROOT)/vspa-sdk/inc
COMMON_INC   = $(VSPA_LIB)/common/include
KERNEL_INC   = $(KERNEL_ROOT)/include

BUILD_DIR ?= build
TARGET     = $(BUILD_DIR)/test_$(KERNEL)_$(ARCH)_$(AU)au.eld

VCFLAGS = \
    -arch $(ARCH) \
    -au_count $(AU) \
    -core_type $(CORE) \
    -ansi off \
    -g -O3 -Os \
    -opt=noalias_by_type \
    -mvcpu \
    -msgstyle gcc \
    -env "$(VSPA_TOOL)" \
    -isystem $(VSPA_TOOL)/include \
    -I$(VSPA_SDK_INC) \
    -I$(COMMON_INC) \
    -I$(KERNEL_INC) \
    $(DEFS) \
    -Xlnk "-Map $(BUILD_DIR)/test_$(KERNEL).map"

.PHONY: all generate test clean

all: $(TARGET)

# Always regenerate vectors before building -- generation is cheap, and this
# avoids silently reusing stale vectors from a previous QAM_MODE (or other
# env-driven gen_vectors.py variant) after switching without `make clean`.
.PHONY: generate
generate:
	python3 gen_vectors.py

$(TARGET): $(SRCS) generate
	@mkdir -p $(BUILD_DIR)
	$(VSPA_CC) $(VCFLAGS) \
	    -mem $(VSPA_TOOL)/etc/$(ARCH)_$(AU)au/lcf/$(ARCH)_$(AU)au_$(CORE).lcf \
	    $(VSPA_TOOL)/lib/$(ARCH)/startup_au$(AU)_$(CORE).eln \
	    $(SRCS) \
	    $(VSPA_TOOL)/lib/$(ARCH)/libc_au$(AU)_$(CORE).elb \
	    $(VSPA_TOOL)/lib/$(ARCH)/librt_au$(AU)_$(CORE).elb \
	    -o $@
	@echo "Built: $@"

test: $(TARGET)
	python3 $(VSPA_LIB)/tools/run_sim.py $(TARGET) $(ARCH)_$(AU)au $(CYCLES)

clean:
	rm -rf $(BUILD_DIR) vectors/input.hex vectors/ref.hex
