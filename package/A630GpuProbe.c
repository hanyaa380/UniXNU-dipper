// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only hardware qualification for the SDM845 Adreno 630. This is the
// first official GPU-porting gate: prove the GCC branches and GPUCC/GDSC
// inputs before enabling clocks, GMU firmware, SMMU or command submission.
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <stddef.h>
#include <vm/memory_types.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern int printf(const char *, ...);

#define GCC_PHYS                 0x00100000UL
#define GCC_SIZE                 0x001f0000UL
#define GCC_APPS_VOTE_0          0x52004
#define GCC_GPU_CFG_AHB          0x71004
#define GCC_GPU_MEMNOC_GFX       0x7100c
#define GCC_GPU_SNOC_DVM_GFX     0x71018

#define GPUCC_PHYS               0x05090000UL
#define GPUCC_SIZE               0x9000UL
#define GPUCC_GX_GDSCR           0x100c
#define GPUCC_CX_GDSCR           0x106c
#define GPUCC_CX_GDSC_HWCTRL     0x1540

#define CBCR_OFF                 (1u << 31)
#define GDSC_PWR_ON              (1u << 31)
#define GDSCR_SW_COLLAPSE        (1u << 0)

volatile uint32_t native_a630_gpu_stage;
volatile uint32_t native_a630_gpu_status;
volatile uint32_t native_a630_gpu_votes;
volatile uint32_t native_a630_gpu_ahb;
volatile uint32_t native_a630_gpu_cx_gdscr;
volatile uint32_t native_a630_gpu_gx_gdscr;

static inline uint32_t
rd(volatile uint8_t *base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static void
branch(const char *name, uint32_t value)
{
	printf("a630gpu: %-24s = %#010x: %s\n", name, value, (value & CBCR_OFF) ? "OFF" : "on");
}

void
native_a630_gpu_probe(void)
{
	uint32_t enabled = 0;
	volatile uint8_t *gcc;

	if (!PE_parse_boot_argn("a630-gpu", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}

	native_a630_gpu_stage = 1;
	gcc = (volatile uint8_t *)ml_io_map(GCC_PHYS, GCC_SIZE);
	if (gcc == NULL) {
		native_a630_gpu_status = 1;
		printf("a630gpu: GCC mapping failed\n");
		return;
	}

	native_a630_gpu_votes = rd(gcc, GCC_APPS_VOTE_0);
	native_a630_gpu_ahb = rd(gcc, GCC_GPU_CFG_AHB);
	printf("a630gpu: GCC apps votes %#010x: GPLL0 %s, GPLL0/2 %s\n", native_a630_gpu_votes,
	    (native_a630_gpu_votes & (1u << 15)) ? "voted" : "not voted",
	    (native_a630_gpu_votes & (1u << 16)) ? "voted" : "not voted");
	branch("gcc_gpu_cfg_ahb_clk", native_a630_gpu_ahb);
	branch("gcc_gpu_memnoc_gfx_clk", rd(gcc, GCC_GPU_MEMNOC_GFX));
	branch("gcc_gpu_snoc_dvm_gfx_clk", rd(gcc, GCC_GPU_SNOC_DVM_GFX));

	if (native_a630_gpu_ahb & CBCR_OFF) {
		native_a630_gpu_status = 2;
		printf("a630gpu: GPU register bus is off; GPUCC is deliberately not mapped\n");
		return;
	}

	{
		volatile uint8_t *gpucc = (volatile uint8_t *)ml_io_map(GPUCC_PHYS, GPUCC_SIZE);

		if (gpucc == NULL) {
			native_a630_gpu_status = 3;
			printf("a630gpu: GPUCC mapping failed\n");
			return;
		}
		native_a630_gpu_stage = 2;
		native_a630_gpu_gx_gdscr = rd(gpucc, GPUCC_GX_GDSCR);
		native_a630_gpu_cx_gdscr = rd(gpucc, GPUCC_CX_GDSCR);
		printf("a630gpu: GX GDSCR %#010x: power %s, software collapse %u\n", native_a630_gpu_gx_gdscr,
		    (native_a630_gpu_gx_gdscr & GDSC_PWR_ON) ? "ON" : "off",
		    native_a630_gpu_gx_gdscr & GDSCR_SW_COLLAPSE);
		printf("a630gpu: CX GDSCR %#010x, hardware control %#010x: power %s, software collapse %u\n",
		    native_a630_gpu_cx_gdscr, rd(gpucc, GPUCC_CX_GDSC_HWCTRL),
		    (rd(gpucc, GPUCC_CX_GDSC_HWCTRL) & GDSC_PWR_ON) ? "ON" : "off",
		    native_a630_gpu_cx_gdscr & GDSCR_SW_COLLAPSE);
	}

	native_a630_gpu_stage = 3;
	native_a630_gpu_status = 0;
	printf("a630gpu: read-only qualification complete; no clock, reset or power register changed\n");
}
