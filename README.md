This repo currently contains two VSPA (Vector Signal Processing Accelerator) kernels for the NXP LA9310 Baseband Processor. Both kernels do BPSK modulation, however one (psk_hw) uses VSPA internal hardware for BPSK modulation and the other (psk) uses standard vector operations. 


Simulator results:\
=======================\
BPSK vectorized non-accelerated - 1084 cycles\
BPSK vectorized accelerated - 96 cycles\
QPSK vectorized accelerated - 99 cycles\

