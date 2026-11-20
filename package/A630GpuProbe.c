// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only hardware qualification for the SDM845 Adreno 630. This is the
// first official GPU-porting gate: prove the GCC branches and GPUCC/GDSC
// inputs before enabling clocks, GMU firmware, SMMU or command submission.
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <kern/clock.h>
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
#define GPUCC_CX_GMU_CBCR        0x1098
#define GPUCC_CXO_CBCR           0x109c
#define GPUCC_GMU_RCG_CMD        0x1120
#define GPUCC_GMU_RCG_CFG        0x1124

#define CBCR_ENABLE              (1u << 0)
#define CBCR_OFF                 (1u << 31)
#define GDSC_PWR_ON              (1u << 31)
#define GDSCR_SW_COLLAPSE        (1u << 0)
#define GDSCR_CLK_DIS_WAIT_MASK  (0xfu << 12)
#define GDSCR_CLK_DIS_WAIT(x)    ((uint32_t)(x) << 12)
#define CX_CLK_DIS_WAIT_VAL      8
#define RCG_CMD_UPDATE           (1u << 0)
#define RCG_CMD_ROOT_EN          (1u << 1)
#define RCG_CMD_ROOT_OFF         (1u << 31)
#define CX_GMU_CBCR_WAKE_SLEEP   ((0xfu << 8) | (0xfu << 4))

volatile uint32_t native_a630_gpu_stage;
volatile uint32_t native_a630_gpu_status;
volatile uint32_t native_a630_gpu_votes;
volatile uint32_t native_a630_gpu_ahb;
volatile uint32_t native_a630_gpu_memnoc;
volatile uint32_t native_a630_gpu_snoc;
volatile uint32_t native_a630_gpu_cx_gdscr;
volatile uint32_t native_a630_gpu_cx_hwctrl;
volatile uint32_t native_a630_gpu_gx_gdscr;
volatile uint32_t native_a630_gpu_rcg_cmd;
volatile uint32_t native_a630_gpu_rcg_cfg;
volatile uint32_t native_a630_gpu_cxo;
volatile uint32_t native_a630_gpu_cx_gmu;

static inline uint32_t
rd(volatile uint8_t *base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static inline void
wr(volatile uint8_t *base, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(base + offset) = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
branch(const char *name, uint32_t value)
{
	printf("a630gpu: %-24s = %#010x: %s\n", name, value, (value & CBCR_OFF) ? "OFF" : "on");
}

static bool
branch_enable(volatile uint8_t *base, uint32_t offset, const char *name)
{
	uint32_t value;

	wr(base, offset, rd(base, offset) | CBCR_ENABLE);
	for (uint32_t waited = 0; waited <= 1000; waited += 5) {
		value = rd(base, offset);
		if (!(value & CBCR_OFF)) {
			printf("a630gpu: %s on: %#010x\n", name, value);
			return true;
		}
		sleep_us(5);
	}
	printf("a630gpu: %s does not come on: %#010x\n", name, rd(base, offset));
	return false;
}

static bool
cx_wait(volatile uint8_t *gpucc, bool on)
{
	for (uint32_t waited = 0; waited <= 2000; waited += 10) {
		if (!!(rd(gpucc, GPUCC_CX_GDSC_HWCTRL) & GDSC_PWR_ON) == on) {
			return true;
		}
		sleep_us(10);
	}
	return false;
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
	native_a630_gpu_memnoc = rd(gcc, GCC_GPU_MEMNOC_GFX);
	native_a630_gpu_snoc = rd(gcc, GCC_GPU_SNOC_DVM_GFX);
	branch("gcc_gpu_memnoc_gfx_clk", native_a630_gpu_memnoc);
	branch("gcc_gpu_snoc_dvm_gfx_clk", native_a630_gpu_snoc);

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
		native_a630_gpu_cx_hwctrl = rd(gpucc, GPUCC_CX_GDSC_HWCTRL);
		printf("a630gpu: GX GDSCR %#010x: power %s, software collapse %u\n", native_a630_gpu_gx_gdscr,
		    (native_a630_gpu_gx_gdscr & GDSC_PWR_ON) ? "ON" : "off",
		    native_a630_gpu_gx_gdscr & GDSCR_SW_COLLAPSE);
		printf("a630gpu: CX GDSCR %#010x, hardware control %#010x: power %s, software collapse %u\n",
		    native_a630_gpu_cx_gdscr, native_a630_gpu_cx_hwctrl,
		    (native_a630_gpu_cx_hwctrl & GDSC_PWR_ON) ? "ON" : "off",
		    native_a630_gpu_cx_gdscr & GDSCR_SW_COLLAPSE);

		if (enabled < 2) {
			native_a630_gpu_status = 0;
			printf("a630gpu: read-only qualification complete; no clock, reset or power register changed\n");
			return;
		}

		native_a630_gpu_stage = 3;
		if (!(native_a630_gpu_cx_gdscr & GDSC_PWR_ON)) {
			uint32_t value = native_a630_gpu_cx_gdscr;

			wr(gpucc, GPUCC_CX_GDSCR,
			    (value & ~GDSCR_CLK_DIS_WAIT_MASK) | GDSCR_CLK_DIS_WAIT(CX_CLK_DIS_WAIT_VAL));
			value = rd(gpucc, GPUCC_CX_GDSCR);
			wr(gpucc, GPUCC_CX_GDSCR, value & ~GDSCR_SW_COLLAPSE);
			sleep_us(1);
			if (!cx_wait(gpucc, true)) {
				native_a630_gpu_status = 4;
				native_a630_gpu_cx_hwctrl = rd(gpucc, GPUCC_CX_GDSC_HWCTRL);
				printf("a630gpu: CX did not power on: GDSCR %#010x hw %#010x\n",
				    rd(gpucc, GPUCC_CX_GDSCR), native_a630_gpu_cx_hwctrl);
				return;
			}
		}
		native_a630_gpu_cx_gdscr = rd(gpucc, GPUCC_CX_GDSCR);
		native_a630_gpu_cx_hwctrl = rd(gpucc, GPUCC_CX_GDSC_HWCTRL);

		bool memnoc_on = branch_enable(gcc, GCC_GPU_MEMNOC_GFX, "gcc_gpu_memnoc_gfx_clk");
		native_a630_gpu_memnoc = rd(gcc, GCC_GPU_MEMNOC_GFX);
		bool snoc_on = branch_enable(gcc, GCC_GPU_SNOC_DVM_GFX, "gcc_gpu_snoc_dvm_gfx_clk");
		native_a630_gpu_snoc = rd(gcc, GCC_GPU_SNOC_DVM_GFX);
		if (!memnoc_on || !snoc_on) {
			native_a630_gpu_status = 5;
			return;
		}
		native_a630_gpu_stage = 4;

		native_a630_gpu_rcg_cfg = rd(gpucc, GPUCC_GMU_RCG_CFG);
		wr(gpucc, GPUCC_GMU_RCG_CFG, native_a630_gpu_rcg_cfg & ~((7u << 8) | 0x1fu));
		native_a630_gpu_stage = 5;
		wr(gpucc, GPUCC_GMU_RCG_CMD,
		    rd(gpucc, GPUCC_GMU_RCG_CMD) | RCG_CMD_ROOT_EN | RCG_CMD_UPDATE);
		for (uint32_t waited = 0; waited <= 1000; waited += 5) {
			uint32_t command = rd(gpucc, GPUCC_GMU_RCG_CMD);
			if (!(command & RCG_CMD_UPDATE) && !(command & RCG_CMD_ROOT_OFF)) {
				break;
			}
			sleep_us(5);
		}
		native_a630_gpu_rcg_cmd = rd(gpucc, GPUCC_GMU_RCG_CMD);
		native_a630_gpu_rcg_cfg = rd(gpucc, GPUCC_GMU_RCG_CFG);
		if ((native_a630_gpu_rcg_cmd & (RCG_CMD_UPDATE | RCG_CMD_ROOT_OFF)) != 0) {
			native_a630_gpu_status = 6;
			printf("a630gpu: GMU clock source did not update: CMD %#010x CFG %#010x\n",
			    native_a630_gpu_rcg_cmd, native_a630_gpu_rcg_cfg);
			return;
		}

		native_a630_gpu_stage = 6;
		native_a630_gpu_cx_gmu = rd(gpucc, GPUCC_CX_GMU_CBCR);
		wr(gpucc, GPUCC_CX_GMU_CBCR,
		    (native_a630_gpu_cx_gmu & ~CX_GMU_CBCR_WAKE_SLEEP) | CX_GMU_CBCR_WAKE_SLEEP);
		bool cxo_on = branch_enable(gpucc, GPUCC_CXO_CBCR, "gpu_cc_cxo_clk");
		native_a630_gpu_cxo = rd(gpucc, GPUCC_CXO_CBCR);
		bool cx_gmu_on = branch_enable(gpucc, GPUCC_CX_GMU_CBCR, "gpu_cc_cx_gmu_clk");
		native_a630_gpu_cx_gmu = rd(gpucc, GPUCC_CX_GMU_CBCR);
		if (!cxo_on || !cx_gmu_on) {
			native_a630_gpu_status = 7;
			return;
		}
	}

	native_a630_gpu_status = 0;
	printf("a630gpu: stage 2 complete: CX power and GMU clocks enabled; GMU firmware/SMMU untouched\n");
}
