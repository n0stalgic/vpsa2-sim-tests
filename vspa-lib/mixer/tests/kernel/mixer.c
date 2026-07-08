#include <cstdint>
#include "vspa/intrinsics.h"
#include "vcpu.h"
#include "vspa.h"

unsigned int mixer_vspa(cfixed16_t *mix_out, cfixed16_t *mix_in,
			uint32_t phase_in, int32_t freq_in, uint32_t L)
{

	unsigned int nco_phase = 0;	
	unsigned int offset_accumulator = 0;

	__clr_VRA();
	__set_prec(half_fixed, half_fixed, half_fixed, single, half_fixed);
	__set_Smode(S0hlinecplx, S1nco, S2zeros);
	__set_creg(255, 8);
	
	__set_nco(normal, 0x1, 0);
	__set_nco_freq(freq_in);
	__set_nco_phase(phase_in);
	
	__set_VRAptr_rS0(_VR0);
	__set_VRAptr_rV(_VR5);
	__set_VRAptr_rSt(5);

	__rd_S2();
	
	__set_VRAincr_rS0(__AU_COUNT__ * 2);
	__set_VRAincr_rV(__AU_COUNT__ * 2);

	for (uint32_t i = 0; i < L; i++)
	{	
		__ld_vec(mix_in + i);
		__ld_Rx(normal, 0);
		__rd_S0();
		__rd_S1();
		__cmad();
		__wr_hlinecplx();

		__rd_S0();
		__rd_S1();
		__cmad();
		__wr_hlinecplx();
		__st_vec((vspa_vector_pair_fixed16 *) mix_out + i);

	}
	
	nco_phase = __get_nco_phase_uint();
	return nco_phase;

	
}

