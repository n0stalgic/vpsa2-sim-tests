#include <cstdint>
#include <vspa/intrinsics.h>
#include "vspa.h"
#include "vcpu.h"
#include "iohw.h"


#define QAM_BPSK	1
#define QAM_QPSK	2
#define QAM_16		4
#define QAM_64		6
#define QAM_256		8
#define QAM_1024	10

#define QAM_NORMALIZATION_BPSK	0x41000000   /* 1 * 8 */
#define QAM_NORMALIZATION_QPSK	0x40B504F3   /* (1/sqrt(2)) * 8  */
#define QAM_NORMALIZATION_16	0x4021E89B   /* (1/sqrt(10)) * 8 */
#define QAM_NORMALIZATION_64	0x3F9E01B3   /* (1/sqrt(42)) * 8 */
#define QAM_NORMALIZATION_256	0x3F1D130E   /* (1/sqrt(170)) * 8 */
#define QAM_NORMALIZATION_1024	0x3E9CD80D   /* (1/sqrt(682)) * 8 */

#pragma optimization_level 0


void mod_qpsk_hw(uint32_t *bit_in, vspa_complex_float16* qpsk_out, uint32_t N)
{

	iowr(LD_RF_CONTROL, 2);
	iowr(LD_RF_TB_REAL_0, 0xFFFFFF22);
	iowr(LD_RF_TB_IMAG_0, 0xFFFFFF0A);
	
	__clr_VRA();
	__set_creg(255,0);
	__set_prec(single, half_fixed, half_fixed, single, half);
	__set_Smode(S0hword, S1straight, S2zeros);	

	__set_VRAptr_rV(_VR0);
	__mv_w_rV(QAM_NORMALIZATION_QPSK);
	__set_VRAptr_rS0(_VR0);
	__rd_S0();
	
	__set_VRAptr_rS1(_VR2);
	
	__rd_S2();

	__set_VRAptr_rSt(0);

	for (uint32_t i = 0; i < N; i++)
	{
		__ld_vec(bit_in + i);
		__ld_Rx(qam, 2);
		__rd_S1();
		__rmad();
		__wr(straight);
		__st_vec((vspa_vector_pair_float16 *) qpsk_out + i);
	}

}

void mod_bpsk_hw(uint32_t* bit_in, vspa_complex_float16* bpsk_out, uint32_t N)
{
	iowr(LD_RF_TB_REAL_0, 0x00000002);
	iowr(LD_RF_CONTROL, 1);

	__clr_VRA();
	__set_creg(255, 0);
	__set_prec(single, half_fixed, half_fixed, single, half);
	__set_Smode(S0i1r1i1r1, S1straight, S2zeros);

	__set_VRAptr_rV(_VR0);
	__mv_w_rV(QAM_NORMALIZATION_BPSK);
	__set_VRAptr_rS0(_VR0);
	__rd_S0();

	__set_VRAptr_rS1(_VR2);

	__rd_S2();

	__set_VRAptr_rSt(0);

	for (uint32_t i = 0; i < N; i++)
	{
		__ld_vec(bit_in + i);
		__ld_Rx(qam, 2);
		__rd_S1();
		__rmad();
		__wr(straight);
		__st_vec((vspa_vector_pair_float16 *) bpsk_out + i);
	}
}

#pragma optimization_level reset
