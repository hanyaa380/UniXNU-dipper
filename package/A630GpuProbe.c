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
#define GCC_GPLL0_MODE           0x0000
#define GCC_GPLL0_L              0x0004
#define GCC_GPLL0_USER           0x000c
#define GCC_GPLL0_FRAC           0x0038

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
#define GMU_RCG_200MHZ_CFG       0x602u

#define GMU_PHYS                 0x0506a000UL
#define GMU_SIZE                 0x30000UL
#define GMU_WORD(w)              ((uint32_t)(w) * 4u)
#define GMU_CM3_SYSRESET_W       0x5000u
#define GMU_CM3_BOOT_CONFIG_W    0x5001u
#define GMU_CM3_FW_INIT_RESULT_W 0x501cu
#define GMU_CM3_CFG_W            0x502du
#define GMU_POWER_COUNTER_EN_W   0x5040u
#define GMU_PWR_COL_KEEPALIVE_W  0x50c3u
#define GMU_RPMH_CTRL_W          0x50e8u
#define GMU_HFI_CTRL_STATUS_W    0x5180u
#define GMU_GENERAL_0_W          0x51c5u
#define GMU_AO_COUNTER_LO_W      0x5088u
#define GMU_AO_COUNTER_HI_W      0x5089u

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
volatile uint32_t native_a630_gpu_gpll0_mode;
volatile uint32_t native_a630_gpu_gpll0_l;
volatile uint32_t native_a630_gpu_gpll0_user;
volatile uint32_t native_a630_gpu_gmu_sysreset;
volatile uint32_t native_a630_gpu_gmu_boot_config;
volatile uint32_t native_a630_gpu_gmu_init_result;
volatile uint32_t native_a630_gpu_gmu_cm3_cfg;
volatile uint32_t native_a630_gpu_gmu_hfi_status;
volatile uint32_t native_a630_gpu_gmu_counter_lo;
volatile uint32_t native_a630_gpu_gmu_counter_hi;
volatile uint32_t native_a630_gpu_gmu_boot_count;
volatile uint32_t native_a630_gpu_gmu_boot_bad;
volatile uint32_t native_a630_gpu_gmu_boot_index;
volatile uint32_t native_a630_gpu_gmu_boot_old;
volatile uint32_t native_a630_gpu_gmu_boot_want;
volatile uint32_t native_a630_gpu_gmu_boot_got;
volatile uint32_t native_a630_gpu_gmu_restore_bad;

static const struct a630_gmu_boot_reg {
	const char *name;
	uint32_t word;
	uint32_t value;
} a630_gmu_boot_regs[] = {
	{ "GMU_GENERAL_7", 0x51cc, 1 },
	{ "GMU_CM3_FW_INIT_RESULT", 0x501c, 0 },
	{ "GMU_CM3_BOOT_CONFIG", 0x5001, 2 },
	{ "GMU_HFI_QTBL_ADDR", 0x5185, 0x60009000 },
	{ "GMU_HFI_QTBL_INFO", 0x5184, 1 },
	{ "GMU_AHB_FENCE_RANGE_0", 0x9311, 0x802800a0 },
	{ "GMU_CM3_CFG", 0x502d, 0x4052 },
	{ "GMU_HFI_SFR_ADDR", 0x5182, 0x6030100 },
	{ "GPU_GMU_CX_GMU_PWR_COL_CP_MSG", 0x5100, 0x60005003 },
	{ "GMU_SYS_BUS_CONFIG", 0x4c0f, 1 },
	{ "GMU_ICACHE_CONFIG", 0x4c00, 1 },
	{ "GMU_DCACHE_CONFIG", 0x4c01, 1 },
	{ "GMU_PWR_COL_INTER_FRAME_CTRL", 0x50c0, 0x09c40400 },
	{ "GMU_RPMH_CTRL", 0x50e8, 0xf11 },
	{ "GPU_GMU_CX_GMU_PWR_COL_CP_RESP", 0x5101, 0 },
};

#define A630_GMU_BOOT_REGS (sizeof(a630_gmu_boot_regs) / sizeof(a630_gmu_boot_regs[0]))

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

		if (enabled < 3) {
			native_a630_gpu_status = 0;
			printf("a630gpu: stage 2 complete: CX power and GMU clocks enabled; GMU firmware/SMMU untouched\n");
			return;
		}

		native_a630_gpu_stage = 7;
		native_a630_gpu_gpll0_mode = rd(gcc, GCC_GPLL0_MODE);
		native_a630_gpu_gpll0_l = rd(gcc, GCC_GPLL0_L);
		native_a630_gpu_gpll0_user = rd(gcc, GCC_GPLL0_USER);
		uint32_t frac = rd(gcc, GCC_GPLL0_FRAC);
		uint64_t hz = 19200000ull * native_a630_gpu_gpll0_l +
		    ((19200000ull * (frac & 0xffff)) >> 16);
		printf("a630gpu: GPLL0 mode %#010x L %#010x frac %#010x user %#010x: %llu Hz, postdiv %u\n",
		    native_a630_gpu_gpll0_mode, native_a630_gpu_gpll0_l, frac,
		    native_a630_gpu_gpll0_user, hz, (native_a630_gpu_gpll0_user >> 8) & 0xf);
		if (!(native_a630_gpu_gpll0_mode & (1u << 31)) ||
		    hz < 599999000 || hz > 600001000 ||
		    ((native_a630_gpu_gpll0_user >> 8) & 0xf) != 1) {
			native_a630_gpu_status = 8;
			printf("a630gpu: GPLL0 prerequisites not confirmed; GMU stays on XO\n");
			return;
		}

		native_a630_gpu_votes = rd(gcc, GCC_APPS_VOTE_0);
		wr(gcc, GCC_APPS_VOTE_0, native_a630_gpu_votes | (1u << 16));
		sleep_us(10);
		native_a630_gpu_votes = rd(gcc, GCC_APPS_VOTE_0);
		if (!(native_a630_gpu_votes & (1u << 16))) {
			native_a630_gpu_status = 9;
			printf("a630gpu: GPLL0/2 vote did not stick: %#010x\n", native_a630_gpu_votes);
			return;
		}

		uint32_t cfg = (native_a630_gpu_rcg_cfg & ~0x71fu) | GMU_RCG_200MHZ_CFG;
		wr(gpucc, GPUCC_GMU_RCG_CFG, cfg);
		wr(gpucc, GPUCC_GMU_RCG_CMD,
		    rd(gpucc, GPUCC_GMU_RCG_CMD) | RCG_CMD_UPDATE);
		bool updated = false;
		for (uint32_t waited = 0; waited <= 1000; waited += 5) {
			uint32_t command = rd(gpucc, GPUCC_GMU_RCG_CMD);
			if (!(command & (RCG_CMD_UPDATE | RCG_CMD_ROOT_OFF))) {
				updated = true;
				break;
			}
			sleep_us(5);
		}
		native_a630_gpu_rcg_cmd = rd(gpucc, GPUCC_GMU_RCG_CMD);
		native_a630_gpu_rcg_cfg = rd(gpucc, GPUCC_GMU_RCG_CFG);
		if (!updated || native_a630_gpu_rcg_cfg != cfg) {
			native_a630_gpu_status = 10;
			printf("a630gpu: GMU 200MHz clock failed: CMD %#010x CFG %#010x want %#010x\n",
			    native_a630_gpu_rcg_cmd, native_a630_gpu_rcg_cfg, cfg);
			return;
		}
		printf("a630gpu: GMU 200MHz clock selected: CMD %#010x CFG %#010x vote %#010x\n",
		    native_a630_gpu_rcg_cmd, native_a630_gpu_rcg_cfg, native_a630_gpu_votes);

		volatile uint8_t *gmu = (volatile uint8_t *)ml_io_map(GMU_PHYS, GMU_SIZE);
		if (gmu == NULL) {
			native_a630_gpu_status = 11;
			printf("a630gpu: GMU mapping failed\n");
			return;
		}
		native_a630_gpu_stage = 8;
		native_a630_gpu_gmu_sysreset = rd(gmu, GMU_WORD(GMU_CM3_SYSRESET_W));
		native_a630_gpu_gmu_boot_config = rd(gmu, GMU_WORD(GMU_CM3_BOOT_CONFIG_W));
		native_a630_gpu_gmu_init_result = rd(gmu, GMU_WORD(GMU_CM3_FW_INIT_RESULT_W));
		native_a630_gpu_gmu_cm3_cfg = rd(gmu, GMU_WORD(GMU_CM3_CFG_W));
		native_a630_gpu_gmu_hfi_status = rd(gmu, GMU_WORD(GMU_HFI_CTRL_STATUS_W));
		native_a630_gpu_gmu_counter_lo = rd(gmu, GMU_WORD(GMU_AO_COUNTER_LO_W));
		sleep_us(1000);
		uint32_t counter = rd(gmu, GMU_WORD(GMU_AO_COUNTER_LO_W));
		native_a630_gpu_gmu_counter_hi = rd(gmu, GMU_WORD(GMU_AO_COUNTER_HI_W));
		printf("a630gpu: GMU SYSRESET %#010x boot %#010x init %#010x CM3_CFG %#010x HFI %#010x\n",
		    native_a630_gpu_gmu_sysreset, native_a630_gpu_gmu_boot_config,
		    native_a630_gpu_gmu_init_result, native_a630_gpu_gmu_cm3_cfg,
		    native_a630_gpu_gmu_hfi_status);
		printf("a630gpu: GMU counter %u -> %u over 1 ms (high %u)\n",
		    native_a630_gpu_gmu_counter_lo, counter, native_a630_gpu_gmu_counter_hi);
		if (counter == native_a630_gpu_gmu_counter_lo) {
			native_a630_gpu_gmu_counter_lo = counter;
			native_a630_gpu_status = 12;
			printf("a630gpu: GMU always-on counter did not advance; 200MHz clock is not proven\n");
			return;
		}
		native_a630_gpu_gmu_counter_lo = counter;

		if (enabled < 4) {
			native_a630_gpu_status = 0;
			printf("a630gpu: stage 3 complete: GMU runs at 200MHz and its registers respond; reset/firmware/SMMU untouched\n");
			return;
		}

		native_a630_gpu_stage = 9;
		if (rd(gmu, GMU_WORD(GMU_CM3_SYSRESET_W)) != 1) {
			native_a630_gpu_status = 13;
			printf("a630gpu: CM3 is not held in reset before boot preflight\n");
			return;
		}

		uint32_t old[A630_GMU_BOOT_REGS];
		uint32_t written = 0;
		for (uint32_t i = 0; i < A630_GMU_BOOT_REGS; i++) {
			uint32_t value = a630_gmu_boot_regs[i].value;
			old[i] = rd(gmu, GMU_WORD(a630_gmu_boot_regs[i].word));
			if (a630_gmu_boot_regs[i].word == GMU_RPMH_CTRL_W) {
				value |= old[i];
			}
			wr(gmu, GMU_WORD(a630_gmu_boot_regs[i].word), value);
			uint32_t got = rd(gmu, GMU_WORD(a630_gmu_boot_regs[i].word));
			written = i + 1;
			printf("a630gpu: boot[%u] %s old %#010x want %#010x got %#010x\n",
			    i, a630_gmu_boot_regs[i].name, old[i], value, got);
			if (got != value) {
				native_a630_gpu_gmu_boot_bad = 1;
				native_a630_gpu_gmu_boot_index = i;
				native_a630_gpu_gmu_boot_old = old[i];
				native_a630_gpu_gmu_boot_want = value;
				native_a630_gpu_gmu_boot_got = got;
				native_a630_gpu_status = 14;
				break;
			}
		}
		native_a630_gpu_gmu_boot_count = written;

		uint32_t restore_bad = 0;
		for (uint32_t i = written; i > 0; i--) {
			uint32_t index = i - 1;
			uint32_t got;

			wr(gmu, GMU_WORD(a630_gmu_boot_regs[index].word), old[index]);
			got = rd(gmu, GMU_WORD(a630_gmu_boot_regs[index].word));
			if (got != old[index]) {
				if (!restore_bad) {
					native_a630_gpu_gmu_boot_index = index;
					native_a630_gpu_gmu_boot_old = old[index];
					native_a630_gpu_gmu_boot_got = got;
				}
				restore_bad++;
				printf("a630gpu: boot restore[%u] %s want %#010x got %#010x\n",
				    index, a630_gmu_boot_regs[index].name, old[index], got);
			}
		}
		native_a630_gpu_gmu_restore_bad = restore_bad;
		native_a630_gpu_gmu_sysreset = rd(gmu, GMU_WORD(GMU_CM3_SYSRESET_W));
		native_a630_gpu_gmu_boot_config = rd(gmu, GMU_WORD(GMU_CM3_BOOT_CONFIG_W));
		native_a630_gpu_gmu_init_result = rd(gmu, GMU_WORD(GMU_CM3_FW_INIT_RESULT_W));
		native_a630_gpu_gmu_cm3_cfg = rd(gmu, GMU_WORD(GMU_CM3_CFG_W));
		if (restore_bad) {
			native_a630_gpu_status = 15;
			printf("a630gpu: GMU boot preflight restore failed: %u mismatches\n", restore_bad);
			return;
		}
		if (native_a630_gpu_gmu_sysreset != 1) {
			native_a630_gpu_status = 16;
			printf("a630gpu: CM3 reset changed during boot preflight\n");
			return;
		}
		native_a630_gpu_stage = 10;
		if (native_a630_gpu_gmu_boot_bad) {
			printf("a630gpu: GMU boot preflight failed at register %u\n",
			    native_a630_gpu_gmu_boot_index);
			return;
		}
		printf("a630gpu: GMU boot preflight: %u registers written, read back and restored; CM3 still held\n",
		    written);
	}

	native_a630_gpu_status = 0;
	printf("a630gpu: stage 4 complete: GMU boot registers are writable; reset/firmware/SMMU untouched\n");
}
