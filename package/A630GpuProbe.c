// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only hardware qualification for the SDM845 Adreno 630. This is the
// first official GPU-porting gate: prove the GCC branches and GPUCC/GDSC
// inputs before enabling clocks, GMU firmware, SMMU or command submission.
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <stddef.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
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
#define GMU_HFI_CTRL_INIT_W      0x5186u
#define GMU_G2H_INTR_CLR_W       0x5191u
#define GMU_G2H_INTR_INFO_W      0x5192u
#define GMU_GMU2HOST_MASK_W      0x5193u
#define GMU_H2G_INTR_SET_W       0x5194u
#define GMU_GENERAL_0_W          0x51c5u
#define GMU_GENERAL_7_W          0x51ccu
#define GMU_AO_COUNTER_LO_W      0x5088u
#define GMU_AO_COUNTER_HI_W      0x5089u
#define GMU_PWR_COL_CP_RESP_W    0x5101u
#define GMU_AO_HOST_STATUS_W     0x9305u
#define GMU_AO_HOST_MASK_W       0x9306u
#define GMU_AO_FENCE_CTRL_W      0x9310u
#define GMU_FENCE_STATUS_W       0x9313u
#define GMU_FENCE_CLR_W          0x9314u
#define GMU_CM3_ITCM_W           0x0c00u
#define GMU_CM3_DTCM_W           0x1c00u
#define A630_GMU_FW_BYTES        0x8000u
#define A630_GMU_FW_WORDS        (A630_GMU_FW_BYTES / 4u)
#define A630_GMU_FW_FNV1A        0xc7fe81d5u
#define A630_GMU_LOG_IOVA        0x60005000ull
#define A630_GMU_HFI_IOVA        0x60009000ull
#define A630_GMU_HFI_BYTES       0x4000u

#define PDC_PHYS                 0x0b280000UL
#define PDC_SEQ_PHYS             0x0b480000UL
#define PDC_SIZE                 0x10000UL
#define RSCC_OFFSET              0x23000u

#define CMD_DB_PHYS              0x85fe0000UL
#define CMD_DB_SIZE              0x20000UL
#define HFI_H2F_MSG_INIT         0u
#define HFI_H2F_MSG_FW_VERSION   1u
#define HFI_H2F_MSG_BW_TABLE     3u
#define HFI_H2F_MSG_PERF_TABLE   4u
#define HFI_H2F_MSG_TEST         5u
#define HFI_F2H_MSG_ERROR        100u
#define HFI_QUEUE_WORDS          1024u

enum a630_rs_region { A630_RS_RSCC, A630_RS_PDC, A630_RS_SEQ };
struct a630_rs_write {
	uint8_t region;
	uint16_t word;
	const char *name;
	uint32_t value;
};

static const struct a630_rs_write a630_rs_writes[] = {
	{ A630_RS_RSCC, 0x0004, "RSC_STATUS0_DRV0", 1u << 24 },
	{ A630_RS_RSCC, 0x000b, "RSCC_PDC_SLAVE_ID_DRV0", 1 },
	{ A630_RS_RSCC, 0x000e, "RSCC_HIDDEN_TCS_CMD0_DATA", 0 },
	{ A630_RS_RSCC, 0x000d, "RSCC_HIDDEN_TCS_CMD0_ADDR", 0 },
	{ A630_RS_RSCC, 0x0010, "RSCC_HIDDEN_TCS_CMD0_DATA+2", 0 },
	{ A630_RS_RSCC, 0x000f, "RSCC_HIDDEN_TCS_CMD0_ADDR+2", 0 },
	{ A630_RS_RSCC, 0x0012, "RSCC_HIDDEN_TCS_CMD0_DATA+4", 0x80000000u },
	{ A630_RS_RSCC, 0x0011, "RSCC_HIDDEN_TCS_CMD0_ADDR+4", 0 },
	{ A630_RS_RSCC, 0x0100, "RSCC_OVERRIDE_START_ADDR", 0 },
	{ A630_RS_RSCC, 0x0008, "RSCC_PDC_SEQ_START_ADDR", 0x4520 },
	{ A630_RS_RSCC, 0x0009, "RSCC_PDC_MATCH_VALUE_LO", 0x4510 },
	{ A630_RS_RSCC, 0x000a, "RSCC_PDC_MATCH_VALUE_HI", 0x4514 },
	{ A630_RS_RSCC, 0x0180, "RSCC_SEQ_MEM_0_DRV0", 0xa7a506a0u },
	{ A630_RS_RSCC, 0x0181, "RSCC_SEQ_MEM_0_DRV0+1", 0xa1e6a6e7u },
	{ A630_RS_RSCC, 0x0182, "RSCC_SEQ_MEM_0_DRV0+2", 0xa2e081e1u },
	{ A630_RS_RSCC, 0x0183, "RSCC_SEQ_MEM_0_DRV0+3", 0xe9a982e2u },
	{ A630_RS_RSCC, 0x0184, "RSCC_SEQ_MEM_0_DRV0+4", 0x0020e8a8u },
	{ A630_RS_SEQ, 0x0000, "PDC_GPU_SEQ_MEM_0", 0xfebea1e1u },
	{ A630_RS_SEQ, 0x0001, "PDC_GPU_SEQ_MEM_0+1", 0xa5a4a3a2u },
	{ A630_RS_SEQ, 0x0002, "PDC_GPU_SEQ_MEM_0+2", 0x8382a6e0u },
	{ A630_RS_SEQ, 0x0003, "PDC_GPU_SEQ_MEM_0+3", 0xbce3e284u },
	{ A630_RS_SEQ, 0x0004, "PDC_GPU_SEQ_MEM_0+4", 0x002081fcu },
	{ A630_RS_PDC, 0x1573, "PDC_GPU_TCS1_CMD_ENABLE_BANK", 7 },
	{ A630_RS_PDC, 0x1574, "PDC_GPU_TCS1_CMD_WAIT_FOR_CMPL_BANK", 0 },
	{ A630_RS_PDC, 0x1572, "PDC_GPU_TCS1_CONTROL", 0 },
	{ A630_RS_PDC, 0x1575, "PDC_GPU_TCS1_CMD0_MSGID", 0x10108 },
	{ A630_RS_PDC, 0x1576, "PDC_GPU_TCS1_CMD0_ADDR", 0x30010 },
	{ A630_RS_PDC, 0x1577, "PDC_GPU_TCS1_CMD0_DATA", 1 },
	{ A630_RS_PDC, 0x1579, "PDC_GPU_TCS1_CMD0_MSGID+4", 0x10108 },
	{ A630_RS_PDC, 0x157a, "PDC_GPU_TCS1_CMD0_ADDR+4", 0x30000 },
	{ A630_RS_PDC, 0x157b, "PDC_GPU_TCS1_CMD0_DATA+4", 0 },
	{ A630_RS_PDC, 0x157d, "PDC_GPU_TCS1_CMD0_MSGID+8", 0x10108 },
	{ A630_RS_PDC, 0x157e, "PDC_GPU_TCS1_CMD0_ADDR+8", 0x30080 },
	{ A630_RS_PDC, 0x157f, "PDC_GPU_TCS1_CMD0_DATA+8", 0 },
	{ A630_RS_PDC, 0x15d7, "PDC_GPU_TCS3_CMD_ENABLE_BANK", 7 },
	{ A630_RS_PDC, 0x15d8, "PDC_GPU_TCS3_CMD_WAIT_FOR_CMPL_BANK", 0 },
	{ A630_RS_PDC, 0x15d6, "PDC_GPU_TCS3_CONTROL", 0 },
	{ A630_RS_PDC, 0x15d9, "PDC_GPU_TCS3_CMD0_MSGID", 0x10108 },
	{ A630_RS_PDC, 0x15da, "PDC_GPU_TCS3_CMD0_ADDR", 0x30010 },
	{ A630_RS_PDC, 0x15db, "PDC_GPU_TCS3_CMD0_DATA", 2 },
	{ A630_RS_PDC, 0x15dd, "PDC_GPU_TCS3_CMD0_MSGID+4", 0x10108 },
	{ A630_RS_PDC, 0x15de, "PDC_GPU_TCS3_CMD0_ADDR+4", 0x30000 },
	{ A630_RS_PDC, 0x15df, "PDC_GPU_TCS3_CMD0_DATA+4", 3 },
	{ A630_RS_PDC, 0x15e1, "PDC_GPU_TCS3_CMD0_MSGID+8", 0x10108 },
	{ A630_RS_PDC, 0x15e2, "PDC_GPU_TCS3_CMD0_ADDR+8", 0x30080 },
	{ A630_RS_PDC, 0x15e3, "PDC_GPU_TCS3_CMD0_DATA+8", 3 },
	{ A630_RS_PDC, 0x1148, "PDC_GPU_SEQ_START_ADDR", 0 },
	{ A630_RS_PDC, 0x1140, "PDC_GPU_ENABLE_PDC", 0x80000001u },
};

#define A630_RS_COUNT (sizeof(a630_rs_writes) / sizeof(a630_rs_writes[0]))

#define SMMU_PHYS                0x05040000UL
#define SMMU_SIZE                0x10000UL
#define SMMU_SCR0                0x000u
#define SMMU_SACR                0x010u
#define SMMU_ID0                 0x020u
#define SMMU_ID1                 0x024u
#define SMMU_ID2                 0x028u
#define SMMU_ID7                 0x03cu
#define SMMU_SGFSR               0x048u
#define SMMU_SGFSYNR0            0x050u
#define SMMU_SMR(n)              (0x800u + 4u * (n))
#define SMMU_S2CR(n)             (0xc00u + 4u * (n))
#define SMMU_GR1                 0x1000u
#define SMMU_CBAR(n)             (SMMU_GR1 + 4u * (n))
#define SMMU_CBA2R(n)            (SMMU_GR1 + 0x800u + 4u * (n))
#define SMMU_CB_SCTLR            0x00u
#define SMMU_CB_TCR2             0x10u
#define SMMU_CB_TTBR0_LO         0x20u
#define SMMU_CB_TTBR0_HI         0x24u
#define SMMU_CB_TTBR1_LO         0x28u
#define SMMU_CB_TTBR1_HI         0x2cu
#define SMMU_CB_TCR              0x30u
#define SMMU_CB_MAIR0            0x38u
#define SMMU_CB_MAIR1            0x3cu
#define SMMU_CB_PAR              0x50u
#define SMMU_CB_FSR              0x58u
#define SMMU_CB_TLBIASID         0x610u
#define SMMU_CB_TLBSYNC          0x7f0u
#define SMMU_CB_TLBSTATUS        0x7f4u
#define SMMU_CB_ATS1PR           0x800u
#define SMMU_CB_ATSR             0x8f0u
#define A630_SMMU_ROWS           16u

#define A630_GMU_CTX             1u
#define A630_GMU_SMR_ENTRY       0u
#define A630_GMU_STREAM          5u
#define A630_SMMU_SMR_VALID      (1u << 31)
#define A630_SMMU_CBAR_S1        (1u << 16 | 3u << 8 | 0xfu << 12)
#define A630_SMMU_CBA2R_VA64     1u
#define A630_SMMU_SCTLR_ON       (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 12)
#define A630_SMMU_TCR_39_BITS    (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define A630_SMMU_TCR2_36_BITS   (1u | 1u << 4 | 7u << 15)
#define A630_SMMU_MAIR0          0xf404ff44u
#define A630_SMMU_FSR_ALL        0xc00001feu
#define A630_PTE_TABLE           3ull
#define A630_PTE_PAGE            3ull
#define A630_PTE_SHARED          (2ull << 8)
#define A630_PTE_ACCESSED        (1ull << 10)
#define A630_PTE_NOT_GLOBAL      (1ull << 11)
#define A630_PTE_ADDRESS         0x0000fffffffff000ull
#define A630_PTE_STAGE1          (A630_PTE_SHARED | A630_PTE_ACCESSED | A630_PTE_NOT_GLOBAL)
#define A630_GMU_IOVA_BASE       0x60000000ull
#define A630_GMU_DMA_BYTES       0x10000u

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
volatile uint32_t native_a630_gpu_smmu_scr0;
volatile uint32_t native_a630_gpu_smmu_sacr;
volatile uint32_t native_a630_gpu_smmu_id0;
volatile uint32_t native_a630_gpu_smmu_id1;
volatile uint32_t native_a630_gpu_smmu_id2;
volatile uint32_t native_a630_gpu_smmu_id7;
volatile uint32_t native_a630_gpu_smmu_sgfsr;
volatile uint32_t native_a630_gpu_smmu_sgfsynr0;
volatile uint32_t native_a630_gpu_smmu_num_smr;
volatile uint32_t native_a630_gpu_smmu_num_cb;
volatile uint32_t native_a630_gpu_smmu_page_size;
volatile uint32_t native_a630_gpu_smmu_cb_base;
volatile uint32_t native_a630_gpu_smmu_used_cb;
volatile uint32_t native_a630_gpu_smmu_smr[A630_SMMU_ROWS];
volatile uint32_t native_a630_gpu_smmu_s2cr[A630_SMMU_ROWS];
volatile uint32_t native_a630_gpu_smmu_cbar[A630_SMMU_ROWS];
volatile uint32_t native_a630_gpu_smmu_cba2r[A630_SMMU_ROWS];
volatile uint32_t native_a630_gpu_smmu_cb_sctlr[A630_SMMU_ROWS];
volatile uint32_t native_a630_gpu_smmu_cb_ttbr0_lo[A630_SMMU_ROWS];
volatile uint32_t native_a630_gpu_smmu_cb_ttbr0_hi[A630_SMMU_ROWS];
volatile uint32_t native_a630_gpu_smmu_cb_tcr[A630_SMMU_ROWS];
volatile uint32_t native_a630_gpu_smmu_cb_mair0[A630_SMMU_ROWS];
volatile uint32_t native_a630_gpu_smmu_cb_fsr[A630_SMMU_ROWS];
volatile uint64_t native_a630_gpu_smmu_gmu_root;
volatile uint64_t native_a630_gpu_smmu_gmu_block;
volatile uint32_t native_a630_gpu_smmu_gmu_pages;
volatile uint32_t native_a630_gpu_smmu_gmu_bad;
volatile uint32_t native_a630_gpu_smmu_gmu_hole_bad;
volatile uint32_t native_a630_gpu_smmu_gmu_smr;
volatile uint32_t native_a630_gpu_smmu_gmu_s2cr;
volatile uint32_t native_a630_gpu_smmu_gmu_cbar;
volatile uint32_t native_a630_gpu_smmu_gmu_sctlr;
volatile uint32_t native_a630_gpu_smmu_gmu_tcr;
volatile uint32_t native_a630_gpu_smmu_gmu_tcr2;
volatile uint32_t native_a630_gpu_smmu_gmu_mair0;
volatile uint64_t native_a630_gpu_smmu_gmu_par;
volatile uint64_t native_a630_gpu_smmu_gmu_hole_par;
volatile uint32_t native_a630_gpu_smmu_gmu_restore_bad;
volatile uint32_t native_a630_gpu_smmu_gmu_restore_mask;
volatile uint32_t native_a630_gpu_smmu_gmu_restore_index;
volatile uint32_t native_a630_gpu_smmu_gmu_restore_old;
volatile uint32_t native_a630_gpu_smmu_gmu_restore_got;
volatile uint32_t native_a630_gpu_smmu_gmu_cbar_sticky;
volatile uint32_t native_a630_gpu_smmu_gmu_s2cr_sticky;
volatile uint32_t native_a630_gpu_gmu_tcm_bad;
volatile uint32_t native_a630_gpu_gmu_tcm_reset;
volatile uint32_t native_a630_gpu_gmu_tcm_itcm_old[2];
volatile uint32_t native_a630_gpu_gmu_tcm_itcm_back[2];
volatile uint32_t native_a630_gpu_gmu_tcm_dtcm_old[2];
volatile uint32_t native_a630_gpu_gmu_tcm_dtcm_back[2];
volatile uint32_t native_a630_gpu_rscc_status0;
volatile uint32_t native_a630_gpu_rscc_seq_busy;
volatile uint32_t native_a630_gpu_pdc_tcs1_control;
volatile uint32_t native_a630_gpu_pdc_tcs1_enable;
volatile uint32_t native_a630_gpu_pdc_tcs1_wait;
volatile uint32_t native_a630_gpu_pdc_tcs3_control;
volatile uint32_t native_a630_gpu_pdc_tcs3_enable;
volatile uint32_t native_a630_gpu_pdc_tcs3_wait;
volatile uint32_t native_a630_gpu_pdc_enable;
volatile uint32_t native_a630_gpu_pdc_seq_start;
volatile uint32_t native_a630_gpu_pdc_seq_mem[5];
volatile uint32_t native_a630_gpu_rsc_write_bad;
volatile uint32_t native_a630_gpu_rsc_restore_bad;
volatile uint32_t native_a630_gpu_rsc_written;
volatile uint32_t native_a630_gpu_rsc_old_enable;
volatile uint32_t native_a630_gpu_rsc_final_enable;
volatile uint32_t native_a630_gpu_gmu_fw_bytes;
volatile uint32_t native_a630_gpu_gmu_fw_hash;
volatile uint32_t native_a630_gpu_gmu_fw_sum;
volatile uint32_t native_a630_gpu_gmu_fw_head[2];
volatile uint32_t native_a630_gpu_gmu_fw_bad;
volatile uint32_t native_a630_gpu_gmu_fw_first_bad;
volatile uint32_t native_a630_gpu_gmu_fw_restore_bad;
volatile uint32_t native_a630_gpu_gmu_fw_reset;
volatile uint32_t native_a630_gpu_gmu_init_mask;
volatile uint32_t native_a630_gpu_gmu_init_want;
volatile uint32_t native_a630_gpu_gmu_cp_resp;
volatile uint32_t native_a630_gpu_gmu_general7;
volatile uint32_t native_a630_gpu_gmu_started;
volatile uint32_t native_a630_gpu_gmu_hfi_started;
volatile uint32_t native_a630_gpu_gmu_hfi_restore_bad;
volatile uint32_t native_a630_gpu_hfi_stage;
volatile uint32_t native_a630_gpu_hfi_version;
volatile uint32_t native_a630_gpu_hfi_last_msg;
volatile uint32_t native_a630_gpu_hfi_last_seq;
volatile uint32_t native_a630_gpu_hfi_last_error;
volatile uint32_t native_a630_gpu_hfi_cmd_db_magic;
volatile uint32_t native_a630_gpu_gmu_boot_restore_bad;
volatile uint32_t native_a630_gpu_gmu_rsc_boot_restore_bad;
volatile uint32_t native_a630_gpu_gmu_tcm_boot_restore_bad;
volatile uint32_t native_a630_gpu_gmu_smmu_boot_restore_bad;
volatile uint32_t native_a630_gpu_gmu_log[4];
static volatile uint8_t *a630_pdc_base;
static volatile uint8_t *a630_pdc_seq_base;

struct a630_hfi_table {
	uint32_t version, size, qhdr0_offset, qhdr_size, num_queues,
	    active_queues;
};

struct a630_hfi_queue {
	uint32_t status, iova, type, size, msg_size, dropped;
	uint32_t rx_watermark, tx_watermark, rx_request, tx_request;
	uint32_t read_index, write_index;
};

_Static_assert(sizeof(struct a630_hfi_table) == 24,
    "A630 HFI table layout");
_Static_assert(sizeof(struct a630_hfi_queue) == 48,
    "A630 HFI queue layout");

struct a630_cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};

struct a630_cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct a630_cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};

struct a630_cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

/*
 * Cloud builds intentionally carry only this marker. scripts/patch-a630-firmware.py
 * replaces the complete array in the downloaded private kernel; vendor firmware
 * must never be committed to the public device or build repositories.
 */
const volatile uint8_t native_a630_gmu_firmware_blob[A630_GMU_FW_BYTES]
    __attribute__((used, section("__DATA,__const"), aligned(64))) =
    "A630-GMU-PLACEHOLDER";
static uint32_t a630_gmu_itcm_backup[A630_GMU_FW_WORDS];

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

struct a630_gmu_smmu_old {
	uint32_t smr, s2cr, cbar, cba2r, sctlr, tcr, tcr2;
	uint32_t ttbr0_lo, ttbr0_hi, ttbr1_lo, ttbr1_hi;
	uint32_t mair0, mair1;
};

static inline uint64_t
rd64(volatile uint8_t *base, uint32_t offset)
{
	return *(volatile uint64_t *)(base + offset);
}

static inline void
wr64(volatile uint8_t *base, uint32_t offset, uint64_t value)
{
	*(volatile uint64_t *)(base + offset) = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
a630_to_memory(vm_offset_t start, vm_size_t size)
{
	for (vm_offset_t line = start & ~(vm_offset_t)63; line < start + size; line += 64) {
		__asm__ volatile("dc civac, %0" : : "r"(line) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static int
a630_gmu_smmu_translate(volatile uint8_t *smmu, uint64_t iova, uint64_t *phys)
{
	volatile uint8_t *cb = smmu + native_a630_gpu_smmu_cb_base +
	    native_a630_gpu_smmu_page_size * A630_GMU_CTX;
	uint64_t par;

	*phys = 0;
	wr64(cb, SMMU_CB_ATS1PR, iova & ~0xfffull);
	for (uint32_t waited = 0; waited < 1000; waited += 10) {
		if (!(rd(cb, SMMU_CB_ATSR) & 1)) {
			par = rd64(cb, SMMU_CB_PAR);
			if (par & 1) {
				*phys = par;
				return 0;
			}
			*phys = par & A630_PTE_ADDRESS;
			return 1;
		}
		sleep_us(10);
	}
	return -1;
}

static uint32_t
a630_gmu_smmu_restore(volatile uint8_t *smmu, vm_offset_t block,
    const struct a630_gmu_smmu_old *old)
{
	volatile uint8_t *cb = smmu + native_a630_gpu_smmu_cb_base +
	    native_a630_gpu_smmu_page_size * A630_GMU_CTX;
	uint32_t bad = 0;

	wr(smmu, SMMU_SMR(A630_GMU_SMR_ENTRY), 0);
	wr(smmu, SMMU_S2CR(A630_GMU_SMR_ENTRY), old->s2cr);
	wr(cb, SMMU_CB_SCTLR, 0);
	__asm__ volatile("dsb sy" ::: "memory");
	wr(cb, SMMU_CB_TLBIASID, A630_GMU_CTX);
	wr(cb, SMMU_CB_TLBSYNC, 1);
	for (uint32_t waited = 0; waited < 1000 &&
	    (rd(cb, SMMU_CB_TLBSTATUS) & 1); waited += 10) {
		sleep_us(10);
	}
	if (rd(smmu, SMMU_SMR(A630_GMU_SMR_ENTRY)) != 0 ||
	    rd(cb, SMMU_CB_SCTLR) != 0 ||
	    (rd(cb, SMMU_CB_TLBSTATUS) & 1)) {
		/* Translation may still be live: keep the tables and DMA pages forever. */
		native_a630_gpu_smmu_gmu_restore_index = 0xffffffffu;
		return 0xffffffffu;
	}

	wr(smmu, SMMU_CBAR(A630_GMU_CTX), old->cbar);
	wr(smmu, SMMU_CBA2R(A630_GMU_CTX), old->cba2r);
	wr(cb, SMMU_CB_TCR, old->tcr);
	wr(cb, SMMU_CB_TCR2, old->tcr2);
	wr(cb, SMMU_CB_TTBR0_LO, old->ttbr0_lo);
	wr(cb, SMMU_CB_TTBR0_HI, old->ttbr0_hi);
	wr(cb, SMMU_CB_TTBR1_LO, old->ttbr1_lo);
	wr(cb, SMMU_CB_TTBR1_HI, old->ttbr1_hi);
	wr(cb, SMMU_CB_MAIR0, old->mair0);
	wr(cb, SMMU_CB_MAIR1, old->mair1);
	wr(cb, SMMU_CB_FSR, A630_SMMU_FSR_ALL);
	wr(smmu, SMMU_S2CR(A630_GMU_SMR_ENTRY), old->s2cr);
	wr(smmu, SMMU_SMR(A630_GMU_SMR_ENTRY), old->smr);
	wr(cb, SMMU_CB_SCTLR, old->sctlr);
	sleep_us(1000);
	__asm__ volatile("dsb sy" ::: "memory");
	bool inactive = rd(cb, SMMU_CB_SCTLR) == 0 &&
	    rd(smmu, SMMU_SMR(A630_GMU_SMR_ENTRY)) == 0 &&
	    rd(cb, SMMU_CB_TTBR0_LO) == 0 &&
	    rd(cb, SMMU_CB_TTBR0_HI) == 0 &&
	    rd(cb, SMMU_CB_TTBR1_LO) == 0 &&
	    rd(cb, SMMU_CB_TTBR1_HI) == 0 &&
	    rd(cb, SMMU_CB_TCR) == 0;

	const struct {
		volatile uint8_t *base;
		uint32_t offset;
		uint32_t value;
	} restored[] = {
		{ smmu, SMMU_SMR(A630_GMU_SMR_ENTRY), old->smr },
		{ smmu, SMMU_S2CR(A630_GMU_SMR_ENTRY), old->s2cr },
		{ smmu, SMMU_CBAR(A630_GMU_CTX), old->cbar },
		{ smmu, SMMU_CBA2R(A630_GMU_CTX), old->cba2r },
		{ cb, SMMU_CB_SCTLR, old->sctlr },
		{ cb, SMMU_CB_TCR, old->tcr },
		{ cb, SMMU_CB_TCR2, old->tcr2 },
		{ cb, SMMU_CB_TTBR0_LO, old->ttbr0_lo },
		{ cb, SMMU_CB_TTBR0_HI, old->ttbr0_hi },
		{ cb, SMMU_CB_TTBR1_LO, old->ttbr1_lo },
		{ cb, SMMU_CB_TTBR1_HI, old->ttbr1_hi },
		{ cb, SMMU_CB_MAIR0, old->mair0 },
		{ cb, SMMU_CB_MAIR1, old->mair1 },
	};
	for (uint32_t i = 0; i < sizeof(restored) / sizeof(restored[0]); i++) {
		uint32_t got = rd(restored[i].base, restored[i].offset);
		if (got != restored[i].value) {
			if ((i == 1 || i == 2) && inactive) {
				/* No SMR can select this context; sticky fields are inert. */
				if (i == 1) {
					native_a630_gpu_smmu_gmu_s2cr_sticky = got;
				} else {
					native_a630_gpu_smmu_gmu_cbar_sticky = got;
				}
				continue;
			}
			native_a630_gpu_smmu_gmu_restore_mask |= 1u << i;
			if (!bad) {
				native_a630_gpu_smmu_gmu_restore_index = i;
				native_a630_gpu_smmu_gmu_restore_old =
				    restored[i].value;
				native_a630_gpu_smmu_gmu_restore_got = got;
			}
			bad++;
		}
	}
	native_a630_gpu_smmu_smr[A630_GMU_SMR_ENTRY] =
	    rd(smmu, SMMU_SMR(A630_GMU_SMR_ENTRY));
	native_a630_gpu_smmu_s2cr[A630_GMU_SMR_ENTRY] =
	    rd(smmu, SMMU_S2CR(A630_GMU_SMR_ENTRY));
	native_a630_gpu_smmu_cbar[A630_GMU_CTX] =
	    rd(smmu, SMMU_CBAR(A630_GMU_CTX));
	native_a630_gpu_smmu_cba2r[A630_GMU_CTX] =
	    rd(smmu, SMMU_CBA2R(A630_GMU_CTX));
	native_a630_gpu_smmu_cb_sctlr[A630_GMU_CTX] =
	    rd(cb, SMMU_CB_SCTLR);
	native_a630_gpu_smmu_cb_ttbr0_lo[A630_GMU_CTX] =
	    rd(cb, SMMU_CB_TTBR0_LO);
	native_a630_gpu_smmu_cb_ttbr0_hi[A630_GMU_CTX] =
	    rd(cb, SMMU_CB_TTBR0_HI);
	native_a630_gpu_smmu_cb_tcr[A630_GMU_CTX] =
	    rd(cb, SMMU_CB_TCR);
	native_a630_gpu_smmu_cb_mair0[A630_GMU_CTX] =
	    rd(cb, SMMU_CB_MAIR0);
	native_a630_gpu_smmu_cb_fsr[A630_GMU_CTX] =
	    rd(cb, SMMU_CB_FSR);
	if (!bad) {
		kmem_free(kernel_map, block, A630_GMU_DMA_BYTES);
	}
	return bad;
}

static bool
a630_gmu_smmu_stage6(volatile uint8_t *smmu)
{
	volatile uint8_t *cb = smmu + native_a630_gpu_smmu_cb_base +
	    native_a630_gpu_smmu_page_size * A630_GMU_CTX;
	struct a630_gmu_smmu_old old = { 0 };
	vm_offset_t block = 0;
	uint64_t *l1, *l2, *l3, root;
	uint32_t bad = 0, hole_bad = 0;

	if (native_a630_gpu_smmu_num_cb <= A630_GMU_CTX ||
	    native_a630_gpu_smmu_num_smr <= A630_GMU_SMR_ENTRY ||
	    rd(smmu, SMMU_SMR(A630_GMU_SMR_ENTRY)) != 0 ||
	    rd(smmu, SMMU_S2CR(A630_GMU_SMR_ENTRY)) != 0x000200ff ||
	    rd(smmu, SMMU_CBAR(A630_GMU_CTX)) != 0 ||
	    rd(smmu, SMMU_CBA2R(A630_GMU_CTX)) != 0 ||
	    rd(cb, SMMU_CB_SCTLR) != 0 ||
	    rd(cb, SMMU_CB_TTBR0_LO) != 0 ||
	    rd(cb, SMMU_CB_TTBR0_HI) != 0) {
		native_a630_gpu_status = 20;
		printf("a630gpu: GPU SMMU GMU route/context is not idle; not written\n");
		return false;
	}

	if (kmem_alloc_contig(kernel_map, &block, A630_GMU_DMA_BYTES, PAGE_MASK,
	    0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		native_a630_gpu_status = 21;
		printf("a630gpu: no 64 KiB contiguous GPU/GMU DMA block\n");
		return false;
	}
	native_a630_gpu_smmu_gmu_block = kvtophys(block);
	l1 = (uint64_t *)block;
	l2 = (uint64_t *)(block + 0x1000);
	l3 = (uint64_t *)(block + 0x2000);
	root = kvtophys(block);
	native_a630_gpu_smmu_gmu_root = root;
	if ((root & 0xfff) ||
	    ((A630_GMU_IOVA_BASE >> 30) != ((A630_GMU_IOVA_BASE + 0xcfff) >> 30)) ||
	    ((A630_GMU_IOVA_BASE >> 21) != ((A630_GMU_IOVA_BASE + 0xcfff) >> 21))) {
		kmem_free(kernel_map, block, A630_GMU_DMA_BYTES);
		native_a630_gpu_status = 22;
		printf("a630gpu: GMU IOVA layout is not in one 2 MiB table or root is unaligned\n");
		return false;
	}
	l1[(A630_GMU_IOVA_BASE >> 30) & 0x1ff] =
	    kvtophys((vm_offset_t)l2) | A630_PTE_TABLE;
	l2[(A630_GMU_IOVA_BASE >> 21) & 0x1ff] =
	    kvtophys((vm_offset_t)l3) | A630_PTE_TABLE;
	for (uint32_t page = 0; page < 13; page++) {
		uint64_t iova = A630_GMU_IOVA_BASE + page * 0x1000ull;
		uint64_t phys = kvtophys(block + 0x3000 + page * 0x1000);

		l3[(iova >> 12) & 0x1ff] = phys | A630_PTE_STAGE1 | A630_PTE_PAGE;
	}
	native_a630_gpu_smmu_gmu_pages = 13;
	a630_to_memory(block, A630_GMU_DMA_BYTES);

	old.smr = rd(smmu, SMMU_SMR(A630_GMU_SMR_ENTRY));
	old.s2cr = rd(smmu, SMMU_S2CR(A630_GMU_SMR_ENTRY));
	old.cbar = rd(smmu, SMMU_CBAR(A630_GMU_CTX));
	old.cba2r = rd(smmu, SMMU_CBA2R(A630_GMU_CTX));
	old.sctlr = rd(cb, SMMU_CB_SCTLR);
	old.tcr = rd(cb, SMMU_CB_TCR);
	old.tcr2 = rd(cb, SMMU_CB_TCR2);
	old.ttbr0_lo = rd(cb, SMMU_CB_TTBR0_LO);
	old.ttbr0_hi = rd(cb, SMMU_CB_TTBR0_HI);
	old.ttbr1_lo = rd(cb, SMMU_CB_TTBR1_LO);
	old.ttbr1_hi = rd(cb, SMMU_CB_TTBR1_HI);
	old.mair0 = rd(cb, SMMU_CB_MAIR0);
	old.mair1 = rd(cb, SMMU_CB_MAIR1);

	native_a630_gpu_stage = 13;
	root |= (uint64_t)A630_GMU_CTX << 48;
	wr(cb, SMMU_CB_SCTLR, 0);
	wr(smmu, SMMU_CBA2R(A630_GMU_CTX), A630_SMMU_CBA2R_VA64);
	wr(smmu, SMMU_CBAR(A630_GMU_CTX), A630_SMMU_CBAR_S1);
	wr(cb, SMMU_CB_TCR2, A630_SMMU_TCR2_36_BITS);
	wr(cb, SMMU_CB_TCR, A630_SMMU_TCR_39_BITS);
	wr(cb, SMMU_CB_TTBR0_LO, (uint32_t)root);
	wr(cb, SMMU_CB_TTBR0_HI, (uint32_t)(root >> 32));
	wr(cb, SMMU_CB_TTBR1_LO, 0);
	wr(cb, SMMU_CB_TTBR1_HI, 0);
	wr(cb, SMMU_CB_MAIR0, A630_SMMU_MAIR0);
	wr(cb, SMMU_CB_MAIR1, 0);
	wr(cb, SMMU_CB_FSR, A630_SMMU_FSR_ALL);
	wr(cb, SMMU_CB_TLBIASID, A630_GMU_CTX);
	wr(cb, SMMU_CB_TLBSYNC, 1);
	for (uint32_t waited = 0; waited < 1000 &&
	    (rd(cb, SMMU_CB_TLBSTATUS) & 1); waited += 10) {
		sleep_us(10);
	}
	if (rd(cb, SMMU_CB_TLBSTATUS) & 1) {
		native_a630_gpu_status = 23;
		printf("a630gpu: GPU SMMU GMU TLB did not quiesce\n");
		native_a630_gpu_smmu_gmu_restore_bad =
		    a630_gmu_smmu_restore(smmu, block, &old);
		return false;
	}
	wr(cb, SMMU_CB_SCTLR, A630_SMMU_SCTLR_ON);
	wr(smmu, SMMU_S2CR(A630_GMU_SMR_ENTRY), A630_GMU_CTX);
	wr(smmu, SMMU_SMR(A630_GMU_SMR_ENTRY),
	    A630_SMMU_SMR_VALID | A630_GMU_STREAM);
	native_a630_gpu_smmu_gmu_smr =
	    rd(smmu, SMMU_SMR(A630_GMU_SMR_ENTRY));
	native_a630_gpu_smmu_gmu_s2cr =
	    rd(smmu, SMMU_S2CR(A630_GMU_SMR_ENTRY));
	native_a630_gpu_smmu_gmu_cbar =
	    rd(smmu, SMMU_CBAR(A630_GMU_CTX));
	native_a630_gpu_smmu_gmu_sctlr = rd(cb, SMMU_CB_SCTLR);
	native_a630_gpu_smmu_gmu_tcr = rd(cb, SMMU_CB_TCR);
	native_a630_gpu_smmu_gmu_tcr2 = rd(cb, SMMU_CB_TCR2);
	native_a630_gpu_smmu_gmu_mair0 = rd(cb, SMMU_CB_MAIR0);
	if (native_a630_gpu_smmu_gmu_smr !=
	    (A630_SMMU_SMR_VALID | A630_GMU_STREAM) ||
	    (native_a630_gpu_smmu_gmu_s2cr & 0x300ff) != A630_GMU_CTX ||
	    !(native_a630_gpu_smmu_gmu_sctlr & 1) ||
	    rd(cb, SMMU_CB_TTBR0_LO) != (uint32_t)root) {
		native_a630_gpu_status = 23;
		printf("a630gpu: GPU SMMU did not take the GMU context\n");
		native_a630_gpu_smmu_gmu_restore_bad =
		    a630_gmu_smmu_restore(smmu, block, &old);
		return false;
	}
	printf("a630gpu: GPU SMMU GMU stream %u -> CB%u: CBAR %#010x SCTLR %#010x TTBR0 %#010x%08x\n",
	    A630_GMU_STREAM, A630_GMU_CTX, native_a630_gpu_smmu_gmu_cbar,
	    native_a630_gpu_smmu_gmu_sctlr, (uint32_t)(root >> 32),
	    (uint32_t)root);

	for (uint32_t page = 0; page < 13; page++) {
		uint64_t iova = A630_GMU_IOVA_BASE + page * 0x1000ull;
		uint64_t expected = kvtophys(block + 0x3000 + page * 0x1000);
		uint64_t phys = 0;

		if (a630_gmu_smmu_translate(smmu, iova, &phys) != 1 ||
		    phys != expected) {
			bad++;
			printf("a630gpu: GMU ATS %#llx wanted %#llx got %#llx\n",
			    iova, expected, phys);
		}
		if (page == 9) {
			native_a630_gpu_smmu_gmu_par = phys;
		}
	}
	const uint64_t holes[] = { 0x60100000ull, 0x00001000ull };
	for (uint32_t i = 0; i < 2; i++) {
		uint64_t phys = 0;

		if (a630_gmu_smmu_translate(smmu, holes[i], &phys) != 0) {
			hole_bad++;
			printf("a630gpu: GMU hole %#llx did not fault: %#llx\n",
			    holes[i], phys);
		}
		if (i == 0) {
			native_a630_gpu_smmu_gmu_hole_par = phys;
		}
	}
	native_a630_gpu_smmu_gmu_bad = bad;
	native_a630_gpu_smmu_gmu_hole_bad = hole_bad;
	uint32_t global_fault = rd(smmu, SMMU_SGFSR);
	if (global_fault) {
		wr(smmu, SMMU_SGFSR, global_fault);
	}
	native_a630_gpu_smmu_gmu_restore_bad =
	    a630_gmu_smmu_restore(smmu, block, &old);
	if (native_a630_gpu_smmu_gmu_restore_bad == 0xffffffffu ||
	    native_a630_gpu_smmu_gmu_restore_bad != 0) {
		native_a630_gpu_status = 25;
		printf("a630gpu: GPU SMMU restore failed (%u); DMA block retained\n",
		    native_a630_gpu_smmu_gmu_restore_bad);
		return false;
	}
	native_a630_gpu_stage = 14;
	native_a630_gpu_smmu_sgfsr = rd(smmu, SMMU_SGFSR);
	native_a630_gpu_smmu_sgfsynr0 = rd(smmu, SMMU_SGFSYNR0);
	if (bad || hole_bad) {
		native_a630_gpu_status = 24;
		printf("a630gpu: GPU SMMU GMU translation failed: %u pages, %u holes\n",
		    bad, hole_bad);
		return false;
	}
	printf("a630gpu: GPU SMMU GMU translation: 13 pages correct, two holes fault, restored\n");
	if (native_a630_gpu_smmu_gmu_s2cr_sticky ||
	    native_a630_gpu_smmu_gmu_cbar_sticky) {
		printf("a630gpu: inert SMMU sticky fields: S2CR %#010x CBAR %#010x\n",
		    native_a630_gpu_smmu_gmu_s2cr_sticky,
		    native_a630_gpu_smmu_gmu_cbar_sticky);
	}
	return true;
}

/*
 * Keep the exact 13-page GMU layout alive across a controlled CM3 boot:
 * 0x60000000 dummy, 0x60001000 debug, 0x60005000 log and 0x60009000 HFI.
 * The stage-6 run leaves S2CR/CBAR sticky but inert; those values are valid
 * input only while SMR, SCTLR and TTBR are all inactive.
 */
static bool
a630_gmu_smmu_boot_map(volatile uint8_t *smmu, vm_offset_t *block_out,
    struct a630_gmu_smmu_old *old)
{
	volatile uint8_t *cb = smmu + native_a630_gpu_smmu_cb_base +
	    native_a630_gpu_smmu_page_size * A630_GMU_CTX;
	vm_offset_t block = 0;
	uint64_t *l1, *l2, *l3, root;
	uint32_t s2cr, cbar, bad = 0;

	if (native_a630_gpu_smmu_num_cb <= A630_GMU_CTX ||
	    native_a630_gpu_smmu_num_smr <= A630_GMU_SMR_ENTRY) {
		native_a630_gpu_status = 38;
		printf("a630gpu: GMU boot SMMU topology is too small\n");
		return false;
	}
	s2cr = rd(smmu, SMMU_S2CR(A630_GMU_SMR_ENTRY));
	cbar = rd(smmu, SMMU_CBAR(A630_GMU_CTX));
	if (rd(smmu, SMMU_SMR(A630_GMU_SMR_ENTRY)) != 0 ||
	    (s2cr != 0x000200ffu && (s2cr & 0x300ffu) != A630_GMU_CTX) ||
	    (cbar != 0 && cbar != A630_SMMU_CBAR_S1) ||
	    rd(cb, SMMU_CB_SCTLR) != 0 ||
	    rd(cb, SMMU_CB_TTBR0_LO) != 0 ||
	    rd(cb, SMMU_CB_TTBR0_HI) != 0) {
		native_a630_gpu_status = 38;
		printf("a630gpu: GMU boot SMMU is not inactive: SMR %#010x S2CR %#010x CBAR %#010x SCTLR %#010x\n",
		    rd(smmu, SMMU_SMR(A630_GMU_SMR_ENTRY)), s2cr, cbar,
		    rd(cb, SMMU_CB_SCTLR));
		return false;
	}
	if (kmem_alloc_contig(kernel_map, &block, A630_GMU_DMA_BYTES, PAGE_MASK,
	    0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		native_a630_gpu_status = 38;
		printf("a630gpu: no persistent 64 KiB GMU boot DMA block\n");
		return false;
	}

	l1 = (uint64_t *)block;
	l2 = (uint64_t *)(block + 0x1000);
	l3 = (uint64_t *)(block + 0x2000);
	root = kvtophys(block);
	if ((root & 0xfff) ||
	    ((A630_GMU_IOVA_BASE >> 30) !=
	    ((A630_GMU_IOVA_BASE + 0xcfff) >> 30)) ||
	    ((A630_GMU_IOVA_BASE >> 21) !=
	    ((A630_GMU_IOVA_BASE + 0xcfff) >> 21))) {
		kmem_free(kernel_map, block, A630_GMU_DMA_BYTES);
		native_a630_gpu_status = 38;
		printf("a630gpu: GMU boot IOVA/root layout is invalid\n");
		return false;
	}
	l1[(A630_GMU_IOVA_BASE >> 30) & 0x1ff] =
	    kvtophys((vm_offset_t)l2) | A630_PTE_TABLE;
	l2[(A630_GMU_IOVA_BASE >> 21) & 0x1ff] =
	    kvtophys((vm_offset_t)l3) | A630_PTE_TABLE;
	for (uint32_t page = 0; page < 13; page++) {
		uint64_t iova = A630_GMU_IOVA_BASE + page * 0x1000ull;
		uint64_t phys = kvtophys(block + 0x3000 + page * 0x1000);

		l3[(iova >> 12) & 0x1ff] =
		    phys | A630_PTE_STAGE1 | A630_PTE_PAGE;
	}
	a630_to_memory(block, A630_GMU_DMA_BYTES);

	old->smr = rd(smmu, SMMU_SMR(A630_GMU_SMR_ENTRY));
	old->s2cr = rd(smmu, SMMU_S2CR(A630_GMU_SMR_ENTRY));
	old->cbar = rd(smmu, SMMU_CBAR(A630_GMU_CTX));
	old->cba2r = rd(smmu, SMMU_CBA2R(A630_GMU_CTX));
	old->sctlr = rd(cb, SMMU_CB_SCTLR);
	old->tcr = rd(cb, SMMU_CB_TCR);
	old->tcr2 = rd(cb, SMMU_CB_TCR2);
	old->ttbr0_lo = rd(cb, SMMU_CB_TTBR0_LO);
	old->ttbr0_hi = rd(cb, SMMU_CB_TTBR0_HI);
	old->ttbr1_lo = rd(cb, SMMU_CB_TTBR1_LO);
	old->ttbr1_hi = rd(cb, SMMU_CB_TTBR1_HI);
	old->mair0 = rd(cb, SMMU_CB_MAIR0);
	old->mair1 = rd(cb, SMMU_CB_MAIR1);

	native_a630_gpu_smmu_gmu_block = kvtophys(block);
	native_a630_gpu_smmu_gmu_root = root;
	native_a630_gpu_smmu_gmu_pages = 13;
	native_a630_gpu_smmu_gmu_bad = 0;
	native_a630_gpu_smmu_gmu_hole_bad = 0;
	native_a630_gpu_smmu_gmu_restore_mask = 0;
	native_a630_gpu_smmu_gmu_s2cr_sticky = 0;
	native_a630_gpu_smmu_gmu_cbar_sticky = 0;

	root |= (uint64_t)A630_GMU_CTX << 48;
	wr(cb, SMMU_CB_SCTLR, 0);
	wr(smmu, SMMU_CBA2R(A630_GMU_CTX), A630_SMMU_CBA2R_VA64);
	wr(smmu, SMMU_CBAR(A630_GMU_CTX), A630_SMMU_CBAR_S1);
	wr(cb, SMMU_CB_TCR2, A630_SMMU_TCR2_36_BITS);
	wr(cb, SMMU_CB_TCR, A630_SMMU_TCR_39_BITS);
	wr(cb, SMMU_CB_TTBR0_LO, (uint32_t)root);
	wr(cb, SMMU_CB_TTBR0_HI, (uint32_t)(root >> 32));
	wr(cb, SMMU_CB_TTBR1_LO, 0);
	wr(cb, SMMU_CB_TTBR1_HI, 0);
	wr(cb, SMMU_CB_MAIR0, A630_SMMU_MAIR0);
	wr(cb, SMMU_CB_MAIR1, 0);
	wr(cb, SMMU_CB_FSR, A630_SMMU_FSR_ALL);
	wr(cb, SMMU_CB_TLBIASID, A630_GMU_CTX);
	wr(cb, SMMU_CB_TLBSYNC, 1);
	for (uint32_t waited = 0; waited < 1000 &&
	    (rd(cb, SMMU_CB_TLBSTATUS) & 1); waited += 10) {
		sleep_us(10);
	}
	if (rd(cb, SMMU_CB_TLBSTATUS) & 1) {
		native_a630_gpu_status = 38;
		printf("a630gpu: GMU boot SMMU TLB did not quiesce\n");
		goto fail;
	}
	wr(cb, SMMU_CB_SCTLR, A630_SMMU_SCTLR_ON);
	wr(smmu, SMMU_S2CR(A630_GMU_SMR_ENTRY), A630_GMU_CTX);
	wr(smmu, SMMU_SMR(A630_GMU_SMR_ENTRY),
	    A630_SMMU_SMR_VALID | A630_GMU_STREAM);
	if (rd(smmu, SMMU_SMR(A630_GMU_SMR_ENTRY)) !=
	    (A630_SMMU_SMR_VALID | A630_GMU_STREAM) ||
	    !(rd(cb, SMMU_CB_SCTLR) & 1) ||
	    rd(cb, SMMU_CB_TTBR0_LO) != (uint32_t)root) {
		native_a630_gpu_status = 38;
		printf("a630gpu: GMU boot SMMU did not activate CB%u\n",
		    A630_GMU_CTX);
		goto fail;
	}
	for (uint32_t page = 0; page < 13; page++) {
		uint64_t iova = A630_GMU_IOVA_BASE + page * 0x1000ull;
		uint64_t expected = kvtophys(block + 0x3000 + page * 0x1000);
		uint64_t phys = 0;

		if (a630_gmu_smmu_translate(smmu, iova, &phys) != 1 ||
		    phys != expected) {
			bad++;
		}
	}
	native_a630_gpu_smmu_gmu_bad = bad;
	if (bad) {
		native_a630_gpu_status = 38;
		printf("a630gpu: GMU boot SMMU translation failed: %u pages\n",
		    bad);
		goto fail;
	}
	*block_out = block;
	printf("a630gpu: GMU boot SMMU live: 13 pages at %#llx root %#llx CB%u\n",
	    (unsigned long long)A630_GMU_IOVA_BASE,
	    (unsigned long long)root, A630_GMU_CTX);
	return true;

fail:
	native_a630_gpu_smmu_gmu_restore_bad =
	    a630_gmu_smmu_restore(smmu, block, old);
	return false;
}

static bool
a630_gmu_tcm_stage7(volatile uint8_t *gmu)
{
	static const uint32_t pattern[2] = { 0xa5a5a5a5u, 0x5a5a5a5au };
	uint32_t bad = 0;

	native_a630_gpu_stage = 15;
	native_a630_gpu_gmu_tcm_reset = rd(gmu, GMU_WORD(GMU_CM3_SYSRESET_W));
	if (native_a630_gpu_gmu_tcm_reset != 1) {
		native_a630_gpu_status = 26;
		printf("a630gpu: GMU TCM test refused: CM3 is not held in reset (%#x)\n",
		    native_a630_gpu_gmu_tcm_reset);
		return false;
	}

	const struct {
		const char *name;
		uint32_t word;
		volatile uint32_t *old;
		volatile uint32_t *back;
	} regions[] = {
		{ "ITCM", GMU_CM3_ITCM_W, native_a630_gpu_gmu_tcm_itcm_old,
		    native_a630_gpu_gmu_tcm_itcm_back },
		{ "DTCM", GMU_CM3_DTCM_W, native_a630_gpu_gmu_tcm_dtcm_old,
		    native_a630_gpu_gmu_tcm_dtcm_back },
	};
	for (uint32_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
		uint32_t offset = GMU_WORD(regions[i].word);

		regions[i].old[0] = rd(gmu, offset);
		regions[i].old[1] = rd(gmu, offset + 4);
		wr(gmu, offset, pattern[0]);
		wr(gmu, offset + 4, pattern[1]);
		regions[i].back[0] = rd(gmu, offset);
		regions[i].back[1] = rd(gmu, offset + 4);
		wr(gmu, offset, regions[i].old[0]);
		wr(gmu, offset + 4, regions[i].old[1]);
		if (regions[i].back[0] != pattern[0] ||
		    regions[i].back[1] != pattern[1]) {
			bad++;
		}
		printf("a630gpu: GMU %s old %#010x/%#010x wrote %#010x/%#010x read %#010x/%#010x restored %#010x/%#010x\n",
		    regions[i].name, regions[i].old[0], regions[i].old[1],
		    pattern[0], pattern[1], regions[i].back[0],
		    regions[i].back[1], rd(gmu, offset), rd(gmu, offset + 4));
	}
	native_a630_gpu_gmu_tcm_bad = bad;
	native_a630_gpu_gmu_tcm_reset = rd(gmu, GMU_WORD(GMU_CM3_SYSRESET_W));
	if (native_a630_gpu_gmu_tcm_reset != 1) {
		native_a630_gpu_status = 26;
		printf("a630gpu: CM3 reset changed during TCM test\n");
		return false;
	}
	if (bad) {
		native_a630_gpu_status = 27;
		printf("a630gpu: GMU TCM test failed: %u regions did not read back\n",
		    bad);
		return false;
	}
	printf("a630gpu: GMU ITCM/DTCM write, readback and restore passed; CM3 still held\n");
	return true;
}

static bool
a630_gmu_rsc_pdc_stage8(volatile uint8_t *gmu)
{
	native_a630_gpu_stage = 16;
	if (rd(gmu, GMU_WORD(GMU_CM3_SYSRESET_W)) != 1) {
		native_a630_gpu_status = 28;
		printf("a630gpu: RSC/PDC read refused: CM3 is not held in reset\n");
		return false;
	}
	volatile uint8_t *pdc = (volatile uint8_t *)ml_io_map(PDC_PHYS, PDC_SIZE);
	volatile uint8_t *seq = (volatile uint8_t *)ml_io_map(PDC_SEQ_PHYS, PDC_SIZE);
	if (pdc == NULL || seq == NULL) {
		native_a630_gpu_status = 29;
		printf("a630gpu: RSC/PDC mapping failed\n");
		return false;
	}
	a630_pdc_base = pdc;
	a630_pdc_seq_base = seq;

	volatile uint8_t *rscc = gmu + RSCC_OFFSET;
	native_a630_gpu_rscc_status0 = rd(rscc, 4u * 0x0004);
	native_a630_gpu_rscc_seq_busy = rd(rscc, 4u * 0x0101);
	native_a630_gpu_pdc_tcs1_control = rd(pdc, 4u * 0x1572);
	native_a630_gpu_pdc_tcs1_enable = rd(pdc, 4u * 0x1573);
	native_a630_gpu_pdc_tcs1_wait = rd(pdc, 4u * 0x1574);
	native_a630_gpu_pdc_tcs3_control = rd(pdc, 4u * 0x15d6);
	native_a630_gpu_pdc_tcs3_enable = rd(pdc, 4u * 0x15d7);
	native_a630_gpu_pdc_tcs3_wait = rd(pdc, 4u * 0x15d8);
	native_a630_gpu_pdc_enable = rd(pdc, 4u * 0x1140);
	native_a630_gpu_pdc_seq_start = rd(pdc, 4u * 0x1148);
	for (uint32_t i = 0; i < 5; i++) {
		native_a630_gpu_pdc_seq_mem[i] = rd(seq, 4u * i);
	}
	if (rd(gmu, GMU_WORD(GMU_CM3_SYSRESET_W)) != 1) {
		native_a630_gpu_status = 28;
		printf("a630gpu: CM3 reset changed during RSC/PDC read\n");
		return false;
	}
	printf("a630gpu: RSC status0 %#010x seq_busy %#010x; PDC TCS1 %#x/%#x/%#x TCS3 %#x/%#x/%#x enable %#010x start %#x\n",
	    native_a630_gpu_rscc_status0, native_a630_gpu_rscc_seq_busy,
	    native_a630_gpu_pdc_tcs1_control, native_a630_gpu_pdc_tcs1_enable,
	    native_a630_gpu_pdc_tcs1_wait, native_a630_gpu_pdc_tcs3_control,
	    native_a630_gpu_pdc_tcs3_enable, native_a630_gpu_pdc_tcs3_wait,
	    native_a630_gpu_pdc_enable, native_a630_gpu_pdc_seq_start);
	printf("a630gpu: PDC sequence memory %#010x %#010x %#010x %#010x %#010x\n",
	    native_a630_gpu_pdc_seq_mem[0], native_a630_gpu_pdc_seq_mem[1],
	    native_a630_gpu_pdc_seq_mem[2], native_a630_gpu_pdc_seq_mem[3],
	    native_a630_gpu_pdc_seq_mem[4]);
	return true;
}

static volatile uint8_t *
a630_rs_address(const struct a630_rs_write *write, volatile uint8_t *rscc)
{
	if (write->region == A630_RS_RSCC) {
		return rscc;
	}
	if (write->region == A630_RS_PDC) {
		return a630_pdc_base;
	}
	return a630_pdc_seq_base;
}

static uint32_t
a630_rs_mask(const struct a630_rs_write *write)
{
	if (write->region == A630_RS_RSCC &&
	    (write->word == 0x000d || write->word == 0x000f ||
	    write->word == 0x0011)) {
		return 0;
	}
	if (write->region == A630_RS_PDC &&
	    (write->word == 0x1575 || write->word == 0x1579 ||
	    write->word == 0x157d || write->word == 0x15d9 ||
	    write->word == 0x15dd || write->word == 0x15e1)) {
		return ~0x8u;
	}
	return ~0u;
}

static bool
a630_rsc_pdc_write_stage9(volatile uint8_t *gmu)
{
	uint32_t old[A630_RS_COUNT];
	uint32_t written = 0, bad = 0, restore_bad = 0;
	volatile uint8_t *rscc = gmu + RSCC_OFFSET;

	native_a630_gpu_stage = 17;
	if (a630_pdc_base == NULL || a630_pdc_seq_base == NULL ||
	    rd(gmu, GMU_WORD(GMU_CM3_SYSRESET_W)) != 1 ||
	    (native_a630_gpu_gx_gdscr & (GDSC_PWR_ON | GDSCR_SW_COLLAPSE)) !=
	    GDSCR_SW_COLLAPSE || rd(rscc, 4u * 0x0101) != 0) {
		native_a630_gpu_status = 30;
		printf("a630gpu: RSC/PDC write refused: mapping, reset, GX or idle precondition failed\n");
		return false;
	}

	native_a630_gpu_rsc_old_enable = rd(a630_pdc_base, 4u * 0x1140);
	wr(a630_pdc_base, 4u * 0x1140,
	    native_a630_gpu_rsc_old_enable & ~1u);
	__asm__ volatile("dsb sy" ::: "memory");
	if (rd(a630_pdc_base, 4u * 0x1140) & 1u) {
		native_a630_gpu_status = 30;
		printf("a630gpu: PDC did not disable for programming\n");
		return false;
	}

	for (uint32_t i = 0; i < A630_RS_COUNT; i++) {
		const struct a630_rs_write *entry = &a630_rs_writes[i];
		volatile uint8_t *base = a630_rs_address(entry, rscc);
		uint32_t offset = 4u * entry->word;
		uint32_t got;

		old[i] = rd(base, offset);
		wr(base, offset, entry->value);
		got = rd(base, offset);
		written = i + 1;
		if (((got ^ entry->value) & a630_rs_mask(entry)) != 0) {
			bad++;
			printf("a630gpu: RSC[%u] %s old %#010x want %#010x got %#010x\n",
			    i, entry->name, old[i], entry->value, got);
			break;
		}
	}
	native_a630_gpu_rsc_written = written;
	native_a630_gpu_rsc_write_bad = bad;

	wr(a630_pdc_base, 4u * 0x1140,
	    rd(a630_pdc_base, 4u * 0x1140) & ~1u);
	__asm__ volatile("dsb sy" ::: "memory");
	for (uint32_t i = written; i > 0; i--) {
		uint32_t index = i - 1;
		const struct a630_rs_write *entry = &a630_rs_writes[index];
		volatile uint8_t *base = a630_rs_address(entry, rscc);
		wr(base, 4u * entry->word, old[index]);
	}
	wr(a630_pdc_base, 4u * 0x1140, native_a630_gpu_rsc_old_enable);
	__asm__ volatile("dsb sy" ::: "memory");
	for (uint32_t i = 0; i < written; i++) {
		const struct a630_rs_write *entry = &a630_rs_writes[i];
		volatile uint8_t *base = a630_rs_address(entry, rscc);
		uint32_t got = rd(base, 4u * entry->word);
		if (((got ^ old[i]) & a630_rs_mask(entry)) != 0) {
			restore_bad++;
		}
	}
	native_a630_gpu_rsc_final_enable = rd(a630_pdc_base, 4u * 0x1140);
	if (native_a630_gpu_rsc_final_enable !=
	    native_a630_gpu_rsc_old_enable) {
		restore_bad++;
	}
	native_a630_gpu_rsc_restore_bad = restore_bad;
	if (restore_bad) {
		native_a630_gpu_status = 33;
		printf("a630gpu: RSC/PDC restore failed: %u mismatches\n",
		    restore_bad);
		return false;
	}
	native_a630_gpu_stage = 18;
	if (bad) {
		native_a630_gpu_status = 32;
		printf("a630gpu: RSC/PDC programming failed at entry %u\n",
		    written - 1);
		return false;
	}
	printf("a630gpu: RSC/PDC 48-entry table written, read back and restored\n");
	return true;
}

static uint32_t
a630_fw_word(const volatile uint8_t *fw, uint32_t index)
{
	const volatile uint8_t *word = fw + 4u * index;

	return (uint32_t)word[0] |
	    (uint32_t)word[1] << 8 |
	    (uint32_t)word[2] << 16 |
	    (uint32_t)word[3] << 24;
}

static uint32_t
a630_fw_fnv1a(const volatile uint8_t *fw, uint32_t bytes)
{
	uint32_t hash = 0x811c9dc5u;

	for (uint32_t i = 0; i < bytes; i++) {
		hash ^= fw[i];
		hash *= 0x01000193u;
	}
	return hash;
}

/*
 * Stage 10 proves the current phone's complete 32 KiB legacy GMU image can
 * enter ITCM and read back. Unlike the production firmware load, this gate
 * restores every original TCM word and keeps CM3 in reset.
 */
static bool
a630_gmu_firmware_stage10(volatile uint8_t *gmu)
{
	volatile uint8_t *itcm = gmu + GMU_WORD(GMU_CM3_ITCM_W);
	uint32_t bytes = sizeof(native_a630_gmu_firmware_blob);
	uint32_t bad = 0, first_bad = 0, restore_bad = 0, sum = 0;

	native_a630_gpu_stage = 19;
	native_a630_gpu_gmu_fw_bytes = bytes;
	native_a630_gpu_gmu_fw_hash =
	    a630_fw_fnv1a(native_a630_gmu_firmware_blob, bytes);
	native_a630_gpu_gmu_fw_head[0] =
	    a630_fw_word(native_a630_gmu_firmware_blob, 0);
	native_a630_gpu_gmu_fw_head[1] =
	    a630_fw_word(native_a630_gmu_firmware_blob, 1);
	native_a630_gpu_gmu_fw_reset =
	    rd(gmu, GMU_WORD(GMU_CM3_SYSRESET_W));

	if (native_a630_gpu_gmu_fw_reset != 1 ||
	    (native_a630_gpu_gx_gdscr & (GDSC_PWR_ON | GDSCR_SW_COLLAPSE)) !=
	    GDSCR_SW_COLLAPSE) {
		native_a630_gpu_status = 30;
		printf("a630gpu: GMU firmware load refused: CM3 reset or GX-off precondition failed\n");
		return false;
	}
	if (bytes != A630_GMU_FW_BYTES ||
	    native_a630_gpu_gmu_fw_hash != A630_GMU_FW_FNV1A) {
		native_a630_gpu_status = 34;
		printf("a630gpu: public kernel has no matching private a630_gmu.bin: %u bytes hash %#010x want %#010x\n",
		    bytes, native_a630_gpu_gmu_fw_hash, A630_GMU_FW_FNV1A);
		return false;
	}

	printf("a630gpu: A630 GMU firmware %u bytes hash %#010x head %08x %08x; backing up ITCM\n",
	    bytes, native_a630_gpu_gmu_fw_hash, native_a630_gpu_gmu_fw_head[0],
	    native_a630_gpu_gmu_fw_head[1]);
	for (uint32_t i = 0; i < A630_GMU_FW_WORDS; i++) {
		a630_gmu_itcm_backup[i] = rd(itcm, 4u * i);
	}
	for (uint32_t i = 0; i < A630_GMU_FW_WORDS; i++) {
		uint32_t word = a630_fw_word(native_a630_gmu_firmware_blob, i);

		wr(itcm, 4u * i, word);
		sum += word;
	}
	native_a630_gpu_gmu_fw_sum = sum;
	native_a630_gpu_gmu_fw_reset =
	    rd(gmu, GMU_WORD(GMU_CM3_SYSRESET_W));
	for (uint32_t i = 0; i < A630_GMU_FW_WORDS; i++) {
		uint32_t want = a630_fw_word(native_a630_gmu_firmware_blob, i);
		uint32_t got = rd(itcm, 4u * i);

		if (got != want) {
			if (bad == 0) {
				first_bad = i;
				printf("a630gpu: firmware word %u got %#010x want %#010x\n",
				    i, got, want);
			}
			bad++;
		}
	}
	native_a630_gpu_gmu_fw_bad = bad;
	native_a630_gpu_gmu_fw_first_bad = first_bad;

	for (uint32_t i = A630_GMU_FW_WORDS; i > 0; i--) {
		uint32_t index = i - 1;

		wr(itcm, 4u * index, a630_gmu_itcm_backup[index]);
	}
	for (uint32_t i = 0; i < A630_GMU_FW_WORDS; i++) {
		if (rd(itcm, 4u * i) != a630_gmu_itcm_backup[i]) {
			restore_bad++;
		}
	}
	native_a630_gpu_gmu_fw_restore_bad = restore_bad;
	native_a630_gpu_gmu_fw_reset =
	    rd(gmu, GMU_WORD(GMU_CM3_SYSRESET_W));

	if (restore_bad) {
		native_a630_gpu_status = 36;
		printf("a630gpu: GMU ITCM restore failed: %u of %u words differ\n",
		    restore_bad, A630_GMU_FW_WORDS);
		return false;
	}
	if (native_a630_gpu_gmu_fw_reset != 1) {
		native_a630_gpu_status = 37;
		printf("a630gpu: CM3 reset changed during firmware qualification: %#010x\n",
		    native_a630_gpu_gmu_fw_reset);
		return false;
	}
	if (bad) {
		native_a630_gpu_status = 35;
		printf("a630gpu: GMU firmware readback failed: %u words differ, first %u\n",
		    bad, first_bad);
		return false;
	}

	native_a630_gpu_status = 0;
	printf("a630gpu: A630 GMU firmware loaded, all %u words read back and ITCM restored; CM3 still held\n",
	    A630_GMU_FW_WORDS);
	return true;
}

static void
a630_gmu_hfi_prepare(vm_offset_t block)
{
	static const uint32_t ids[] = { 0, 4, 5 };
	uint8_t *hfi = (uint8_t *)(block + 0x3000 + 9 * 0x1000);
	struct a630_hfi_table *table = (struct a630_hfi_table *)hfi;
	struct a630_hfi_queue *queue =
	    (struct a630_hfi_queue *)(hfi + sizeof(*table));

	for (uint32_t i = 0; i < A630_GMU_HFI_BYTES / 4; i++) {
		((uint32_t *)hfi)[i] = 0;
	}
	table->version = 0;
	table->size = sizeof(*table) + 3 * sizeof(*queue);
	table->qhdr0_offset = sizeof(*table) / 4;
	table->qhdr_size = sizeof(*queue) / 4;
	table->num_queues = 3;
	table->active_queues = 3;
	for (uint32_t i = 0; i < 3; i++) {
		queue[i].status = 1;
		queue[i].iova = (uint32_t)A630_GMU_HFI_IOVA +
		    0x1000 * (i + 1);
		queue[i].type = (10u << 8) | ids[i];
		queue[i].size = 0x1000 / 4;
		queue[i].rx_watermark = 1;
		queue[i].tx_watermark = 1;
		queue[i].rx_request = 1;
	}
	a630_to_memory((vm_offset_t)hfi, A630_GMU_HFI_BYTES);
	printf("a630gpu: GMU HFI table ready at %#llx: size %u, %u queues\n",
	    (unsigned long long)A630_GMU_HFI_IOVA, table->size,
	    table->active_queues);
}

static vm_offset_t a630_cmd_db_va;

static const struct a630_cmd_db_header *
a630_cmd_db(void)
{
	const struct a630_cmd_db_header *db;

	if (a630_cmd_db_va == 0) {
		a630_cmd_db_va = ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
	}
	if (a630_cmd_db_va == 0) {
		return NULL;
	}
	db = (const struct a630_cmd_db_header *)a630_cmd_db_va;
	native_a630_gpu_hfi_cmd_db_magic =
	    (uint32_t)db->magic[0] | (uint32_t)db->magic[1] << 8 |
	    (uint32_t)db->magic[2] << 16 | (uint32_t)db->magic[3] << 24;
	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 ||
	    db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("a630gpu: RPMh command DB magic is %08x\n",
		    native_a630_gpu_hfi_cmd_db_magic);
		return NULL;
	}
	return db;
}

static bool
a630_cmd_id_equal(const char *id, const char *name)
{
	for (uint32_t i = 0; i < 8; i++) {
		if (id[i] != name[i]) {
			return false;
		}
		if (name[i] == 0) {
			return true;
		}
	}
	return true;
}

static const uint16_t *
a630_rpmh_levels(const struct a630_cmd_db_header *db, const char *name,
    uint32_t *count)
{
	for (uint32_t slave_index = 0; slave_index < 8; slave_index++) {
		const struct a630_cmd_db_slave *slave =
		    &db->slave[slave_index];
		const struct a630_cmd_db_entry *entries;

		if (slave->id == 0 || slave->header_offset >=
		    CMD_DB_SIZE - 0x1000 || slave->count > 512 ||
		    (uint32_t)slave->header_offset +
		    (uint32_t)slave->count *
		    sizeof(struct a630_cmd_db_entry) >
		    CMD_DB_SIZE - 0x1000) {
			continue;
		}
		entries = (const struct a630_cmd_db_entry *)
		    (db->data + slave->header_offset);
		for (uint32_t i = 0; i < slave->count; i++) {
			uint32_t offset;

			if (!a630_cmd_id_equal(entries[i].id, name)) {
				continue;
			}
			if (entries[i].length < 2 ||
			    entries[i].length > 64 ||
			    (entries[i].length & 1) != 0) {
				return NULL;
			}
			offset = (uint32_t)slave->data_offset +
			    entries[i].offset;
			if (offset + entries[i].length >
			    CMD_DB_SIZE - 0x1000) {
				return NULL;
			}
			*count = entries[i].length / 2;
			return (const uint16_t *)(db->data + offset);
		}
	}
	return NULL;
}

static bool
a630_arc_vote(const struct a630_cmd_db_header *db, const char *primary_name,
    const char *secondary_name, uint32_t level, uint32_t *vote)
{
	const uint16_t *primary, *secondary;
	uint32_t primary_count = 0, secondary_count = 0;
	uint32_t primary_index = 0, secondary_index = 0, i;

	primary = a630_rpmh_levels(db, primary_name, &primary_count);
	secondary = a630_rpmh_levels(db, secondary_name, &secondary_count);
	if (primary == NULL || secondary == NULL) {
		return false;
	}
	for (i = 0; i < primary_count; i++) {
		if (primary[i] >= level) {
			primary_index = i;
			break;
		}
	}
	if (i == primary_count) {
		return false;
	}
	for (i = 0; i < secondary_count; i++) {
		if (secondary[i] >= level) {
			secondary_index = i;
			break;
		}
		if (secondary[i] != 0) {
			secondary_index = i;
		}
	}
	*vote = ((uint32_t)primary[primary_index] << 16) |
	    (secondary_index << 8) | primary_index;
	return true;
}

static struct a630_hfi_queue *
a630_hfi_header(vm_offset_t block, uint32_t queue)
{
	uint8_t *hfi = (uint8_t *)(block + 0x3000 + 9 * 0x1000);

	return (struct a630_hfi_queue *)(hfi +
	    sizeof(struct a630_hfi_table)) + queue;
}

static uint32_t *
a630_hfi_data(vm_offset_t block, uint32_t queue)
{
	uint8_t *hfi = (uint8_t *)(block + 0x3000 + 9 * 0x1000);

	return (uint32_t *)(hfi + 0x1000 * (queue + 1));
}

static uint32_t a630_hfi_seqnum;

static bool
a630_hfi_send(volatile uint8_t *gmu, vm_offset_t block, uint32_t id,
    uint32_t *message, uint32_t dwords, uint32_t *payload,
    uint32_t payload_words)
{
	uint8_t *hfi = (uint8_t *)(block + 0x3000 + 9 * 0x1000);
	struct a630_hfi_queue *command = a630_hfi_header(block, 0);
	struct a630_hfi_queue *response = a630_hfi_header(block, 1);
	uint32_t *command_data = a630_hfi_data(block, 0);
	uint32_t *response_data = a630_hfi_data(block, 1);
	uint32_t sequence = ++a630_hfi_seqnum % 0xfff;
	uint32_t index, space;

	a630_to_memory((vm_offset_t)hfi, 0x1000);
	index = command->write_index;
	space = (command->read_index - index - 1) & (HFI_QUEUE_WORDS - 1);
	if (space < dwords) {
		printf("a630gpu: HFI command queue full for message %u\n", id);
		return false;
	}
	message[0] = (sequence << 20) | (dwords << 8) | id;
	for (uint32_t i = 0; i < dwords; i++) {
		command_data[index] = message[i];
		index = (index + 1) & (HFI_QUEUE_WORDS - 1);
	}
	a630_to_memory((vm_offset_t)command_data, 0x1000);
	command->write_index = index;
	a630_to_memory((vm_offset_t)hfi, 0x1000);
	wr(gmu, GMU_WORD(GMU_H2G_INTR_SET_W), 1);
	__asm__ volatile("dsb sy" ::: "memory");

	native_a630_gpu_hfi_last_msg = id;
	native_a630_gpu_hfi_last_seq = sequence;
	native_a630_gpu_hfi_last_error = 0;
	for (uint32_t round = 0; round < 3; round++) {
		bool interrupted = false;

		for (uint32_t i = 0; i < 5000; i++) {
			if (rd(gmu, GMU_WORD(GMU_G2H_INTR_INFO_W)) & 1u) {
				interrupted = true;
				break;
			}
			sleep_us(100);
		}
		if (!interrupted) {
			printf("a630gpu: HFI message %u seq %u timed out; INTR %#x\n",
			    id, sequence,
			    rd(gmu, GMU_WORD(GMU_G2H_INTR_INFO_W)));
			return false;
		}
		wr(gmu, GMU_WORD(GMU_G2H_INTR_CLR_W), 1);
		for (;;) {
			uint32_t reply[19], header, count, read;

			a630_to_memory((vm_offset_t)hfi, 0x1000);
			a630_to_memory((vm_offset_t)response_data, 0x1000);
			if (response->read_index == response->write_index) {
				break;
			}
			read = response->read_index;
			header = response_data[read];
			count = (header >> 8) & 0xff;
			if (count == 0 || count > 19) {
				printf("a630gpu: HFI response header %#x has count %u\n",
				    header, count);
				return false;
			}
			for (uint32_t i = 0; i < count; i++) {
				reply[i] = response_data[read];
				read = (read + 1) & (HFI_QUEUE_WORDS - 1);
			}
			response->read_index = read;
			a630_to_memory((vm_offset_t)hfi, 0x1000);
			if ((header & 0xff) == HFI_F2H_MSG_ERROR) {
				native_a630_gpu_hfi_last_error =
				    count > 1 ? reply[1] : 0xffffffffu;
				printf("a630gpu: HFI firmware error %u\n",
				    native_a630_gpu_hfi_last_error);
				continue;
			}
			if (count < 3 ||
			    ((reply[1] >> 20) & 0xfff) != sequence) {
				continue;
			}
			native_a630_gpu_hfi_last_error = reply[2];
			if (reply[2] != 0) {
				printf("a630gpu: HFI message %u returned error %u\n",
				    id, reply[2]);
				return false;
			}
			if (payload != NULL) {
				for (uint32_t i = 0;
				    i < payload_words && 3 + i < count; i++) {
					payload[i] = reply[3 + i];
				}
			}
			printf("a630gpu: HFI message %u seq %u acknowledged\n",
			    id, sequence);
			return true;
		}
	}
	printf("a630gpu: HFI message %u got no response\n", id);
	return false;
}

static const struct {
	uint32_t khz, level;
} a630_gpu_perf[] = {
	{ 0, 0 }, { 257000, 64 }, { 342000, 128 }, { 414000, 192 },
	{ 520000, 256 }, { 596000, 320 }, { 675000, 384 },
	{ 710000, 416 },
};

static const struct {
	uint32_t khz, level;
} a630_gmu_perf[] = { { 0, 0 }, { 200000, 48 }, { 400000, 128 } };

static bool
a630_hfi_handshake(volatile uint8_t *gmu, vm_offset_t block)
{
	const struct a630_cmd_db_header *db = a630_cmd_db();
	uint32_t init[5] = { 0 }, version_request[2] = { 0 };
	uint32_t test[1] = { 0 }, version = 0;
	uint32_t perf[43] = { 0 }, bandwidth[160] = { 0 };

	if (db == NULL) {
		return false;
	}
	a630_hfi_seqnum = 0;
	wr(gmu, GMU_WORD(GMU_G2H_INTR_CLR_W),
	    rd(gmu, GMU_WORD(GMU_G2H_INTR_INFO_W)));
	__asm__ volatile("dsb sy" ::: "memory");
	native_a630_gpu_hfi_stage = 2;
	init[2] = 0x60001000u;
	init[3] = 0x4000u;
	init[4] = 1;
	if (!a630_hfi_send(gmu, block, HFI_H2F_MSG_INIT, init, 5,
	    NULL, 0)) {
		return false;
	}

	native_a630_gpu_hfi_stage = 3;
	version_request[1] = (1u << 28) | (1u << 19) | (1u << 17);
	if (!a630_hfi_send(gmu, block, HFI_H2F_MSG_FW_VERSION,
	    version_request, 2, &version, 1)) {
		return false;
	}
	native_a630_gpu_hfi_version = version;
	printf("a630gpu: GMU HFI firmware version %#010x\n", version);

	native_a630_gpu_hfi_stage = 4;
	perf[1] = sizeof(a630_gpu_perf) / sizeof(a630_gpu_perf[0]);
	perf[2] = sizeof(a630_gmu_perf) / sizeof(a630_gmu_perf[0]);
	for (uint32_t i = 0; i < perf[1]; i++) {
		if (!a630_arc_vote(db, "gfx.lvl", "mx.lvl",
		    a630_gpu_perf[i].level, &perf[3 + 2 * i])) {
			printf("a630gpu: no gfx.lvl vote for %u\n",
			    a630_gpu_perf[i].level);
			return false;
		}
		perf[4 + 2 * i] = a630_gpu_perf[i].khz;
	}
	for (uint32_t i = 0; i < perf[2]; i++) {
		if (!a630_arc_vote(db, "cx.lvl", "mx.lvl",
		    a630_gmu_perf[i].level, &perf[35 + 2 * i])) {
			printf("a630gpu: no cx.lvl vote for %u\n",
			    a630_gmu_perf[i].level);
			return false;
		}
		perf[36 + 2 * i] = a630_gmu_perf[i].khz;
	}
	if (!a630_hfi_send(gmu, block, HFI_H2F_MSG_PERF_TABLE, perf, 43,
	    NULL, 0)) {
		return false;
	}

	native_a630_gpu_hfi_stage = 5;
	bandwidth[1] = 1;
	bandwidth[2] = 3;
	bandwidth[3] = 3;
	bandwidth[4] = 0x05;
	bandwidth[5] = 0x07;
	bandwidth[6] = 0x50034;
	bandwidth[7] = 0x5007c;
	bandwidth[8] = 0x5004c;
	bandwidth[12] = 0x40000000;
	bandwidth[13] = 0;
	bandwidth[14] = 0x40000000;
	bandwidth[18] = 0x60000001;
	bandwidth[19] = 0x20000001;
	bandwidth[20] = 0x60000001;
	bandwidth[24] = 0x50000;
	bandwidth[25] = 0x5005c;
	bandwidth[26] = 0x5000c;
	bandwidth[32] = 0x40000000;
	bandwidth[33] = 0x40000000;
	bandwidth[34] = 0x40000000;
	if (!a630_hfi_send(gmu, block, HFI_H2F_MSG_BW_TABLE, bandwidth,
	    160, NULL, 0)) {
		return false;
	}

	native_a630_gpu_hfi_stage = 6;
	if (!a630_hfi_send(gmu, block, HFI_H2F_MSG_TEST, test, 1,
	    NULL, 0)) {
		return false;
	}
	native_a630_gpu_hfi_stage = 7;
	printf("a630gpu: HFI INIT, version, perf, bandwidth and TEST handshake complete\n");
	return true;
}

static uint32_t
a630_gmu_boot_restore(volatile uint8_t *gmu, const uint32_t *old,
    uint32_t written)
{
	uint32_t bad = 0;

	for (uint32_t i = written; i > 0; i--) {
		uint32_t index = i - 1;
		uint32_t got;

		wr(gmu, GMU_WORD(a630_gmu_boot_regs[index].word), old[index]);
		got = rd(gmu, GMU_WORD(a630_gmu_boot_regs[index].word));
		if (got != old[index]) {
			bad++;
			printf("a630gpu: boot cleanup[%u] %s want %#010x got %#010x\n",
			    index, a630_gmu_boot_regs[index].name, old[index],
			    got);
		}
	}
	return bad;
}

static uint32_t
a630_gmu_boot_rsc_restore(volatile uint8_t *gmu, const uint32_t *old,
    uint32_t written, uint32_t old_enable)
{
	volatile uint8_t *rscc = gmu + RSCC_OFFSET;
	uint32_t bad = 0;

	wr(a630_pdc_base, 4u * 0x1140,
	    rd(a630_pdc_base, 4u * 0x1140) & ~1u);
	__asm__ volatile("dsb sy" ::: "memory");
	if (rd(a630_pdc_base, 4u * 0x1140) & 1u) {
		printf("a630gpu: PDC did not disable for Stage 11 cleanup\n");
		return written + 1;
	}
	for (uint32_t i = written; i > 0; i--) {
		uint32_t index = i - 1;
		const struct a630_rs_write *entry = &a630_rs_writes[index];
		volatile uint8_t *base = a630_rs_address(entry, rscc);
		uint32_t got;

		wr(base, 4u * entry->word, old[index]);
		got = rd(base, 4u * entry->word);
		if (((got ^ old[index]) & a630_rs_mask(entry)) != 0) {
			bad++;
		}
	}
	wr(a630_pdc_base, 4u * 0x1140, old_enable);
	__asm__ volatile("dsb sy" ::: "memory");
	if (rd(a630_pdc_base, 4u * 0x1140) != old_enable) {
		bad++;
	}
	return bad;
}

static uint32_t
a630_gmu_boot_tcm_restore(volatile uint8_t *gmu)
{
	volatile uint8_t *itcm = gmu + GMU_WORD(GMU_CM3_ITCM_W);
	uint32_t bad = 0;

	for (uint32_t i = A630_GMU_FW_WORDS; i > 0; i--) {
		uint32_t index = i - 1;

		wr(itcm, 4u * index, a630_gmu_itcm_backup[index]);
	}
	for (uint32_t i = 0; i < A630_GMU_FW_WORDS; i++) {
		if (rd(itcm, 4u * i) != a630_gmu_itcm_backup[i]) {
			bad++;
		}
	}
	return bad;
}

static bool
a630_gmu_boot_rsc_program(volatile uint8_t *gmu, uint32_t *old,
    uint32_t *old_enable, uint32_t *written_out, uint32_t *bad_out)
{
	volatile uint8_t *rscc = gmu + RSCC_OFFSET;
	uint32_t written = 0, bad = 0;

	if (a630_pdc_base == NULL || a630_pdc_seq_base == NULL ||
	    rd(gmu, GMU_WORD(GMU_CM3_SYSRESET_W)) != 1 ||
	    rd(rscc, 4u * 0x0101) != 0) {
		printf("a630gpu: Stage 11 RSC/PDC prerequisites failed\n");
		return false;
	}
	*old_enable = rd(a630_pdc_base, 4u * 0x1140);
	wr(a630_pdc_base, 4u * 0x1140, *old_enable & ~1u);
	__asm__ volatile("dsb sy" ::: "memory");
	if (rd(a630_pdc_base, 4u * 0x1140) & 1u) {
		printf("a630gpu: PDC did not disable for Stage 11 boot\n");
		return false;
	}
	for (uint32_t i = 0; i < A630_RS_COUNT; i++) {
		const struct a630_rs_write *entry = &a630_rs_writes[i];
		volatile uint8_t *base = a630_rs_address(entry, rscc);
		uint32_t got;

		old[i] = rd(base, 4u * entry->word);
		wr(base, 4u * entry->word, entry->value);
		got = rd(base, 4u * entry->word);
		written = i + 1;
		if (((got ^ entry->value) & a630_rs_mask(entry)) != 0) {
			bad++;
			printf("a630gpu: Stage 11 RSC[%u] %s want %#010x got %#010x\n",
			    i, entry->name, entry->value, got);
			break;
		}
	}
	*written_out = written;
	*bad_out = bad;
	native_a630_gpu_rsc_written = written;
	native_a630_gpu_rsc_write_bad = bad;
	native_a630_gpu_rsc_old_enable = *old_enable;
	native_a630_gpu_rsc_final_enable =
	    rd(a630_pdc_base, 4u * 0x1140);
	if (bad || rd(rscc, 4u * 0x0101) != 0) {
		return false;
	}
	return true;
}

/*
 * Stage 11 performs one controlled legacy-GMU cold start with GX off.
 * Every resource needed by the firmware is live until CM3 is held again:
 * SMMU pages, HFI tables, ITCM, boot registers and RSC/PDC tables.
 */
static bool
a630_gmu_boot_stage11(volatile uint8_t *gmu, volatile uint8_t *smmu,
    uint32_t hfi_mode)
{
	struct a630_gmu_smmu_old smmu_old = { 0 };
	uint32_t boot_old[A630_GMU_BOOT_REGS] = { 0 };
	uint32_t rsc_old[A630_RS_COUNT] = { 0 };
	uint32_t boot_written = 0, rsc_written = 0;
	uint32_t rsc_old_enable = 0, rsc_bad = 0;
	volatile uint8_t *log = NULL;
	vm_offset_t block = 0;
	uint32_t bytes = sizeof(native_a630_gmu_firmware_blob);
	uint32_t fw_bad = 0, fw_first = 0, fw_sum = 0;
	uint32_t old_ao_mask = 0, old_gmu_mask = 0;
	uint32_t old_hfi_init = 0;
	uint32_t selector, result = 0, restore_bad = 0;
	uint32_t failure = 0;
	bool mapped = false, tcm_backed = false, rsc_touched = false;
	bool masks_changed = false, hfi_control_touched = false;
	bool released = false, halted = true;
	bool started = false;

	native_a630_gpu_stage = 20 + hfi_mode;
	native_a630_gpu_gmu_hfi_started = 0;
	native_a630_gpu_hfi_stage = 0;
	native_a630_gpu_gmu_fw_bytes = bytes;
	native_a630_gpu_gmu_boot_bad = 0;
	native_a630_gpu_gmu_fw_hash =
	    a630_fw_fnv1a(native_a630_gmu_firmware_blob, bytes);
	native_a630_gpu_gmu_fw_head[0] =
	    a630_fw_word(native_a630_gmu_firmware_blob, 0);
	native_a630_gpu_gmu_fw_head[1] =
	    a630_fw_word(native_a630_gmu_firmware_blob, 1);
	if (bytes != A630_GMU_FW_BYTES ||
	    native_a630_gpu_gmu_fw_hash != A630_GMU_FW_FNV1A) {
		native_a630_gpu_status = 34;
		printf("a630gpu: Stage 11 has no matching private GMU firmware\n");
		return false;
	}
	if (rd(gmu, GMU_WORD(GMU_CM3_SYSRESET_W)) != 1 ||
	    (native_a630_gpu_gx_gdscr & (GDSC_PWR_ON | GDSCR_SW_COLLAPSE)) !=
	    GDSCR_SW_COLLAPSE) {
		native_a630_gpu_status = 38;
		printf("a630gpu: Stage 11 refused: CM3 reset or GX-off precondition failed\n");
		return false;
	}
	if (!a630_gmu_smmu_boot_map(smmu, &block, &smmu_old)) {
		return false;
	}
	mapped = true;
	a630_gmu_hfi_prepare(block);

	for (uint32_t i = 0; i < A630_GMU_FW_WORDS; i++) {
		a630_gmu_itcm_backup[i] =
		    rd(gmu, GMU_WORD(GMU_CM3_ITCM_W) + 4u * i);
	}
	tcm_backed = true;
	for (uint32_t i = 0; i < A630_GMU_FW_WORDS; i++) {
		uint32_t word = a630_fw_word(native_a630_gmu_firmware_blob, i);

		wr(gmu, GMU_WORD(GMU_CM3_ITCM_W) + 4u * i, word);
		fw_sum += word;
	}
	for (uint32_t i = 0; i < A630_GMU_FW_WORDS; i++) {
		uint32_t want = a630_fw_word(native_a630_gmu_firmware_blob, i);
		uint32_t got = rd(gmu, GMU_WORD(GMU_CM3_ITCM_W) + 4u * i);

		if (got != want) {
			if (!fw_bad) {
				fw_first = i;
				printf("a630gpu: Stage 11 firmware word %u got %#010x want %#010x\n",
				    i, got, want);
			}
			fw_bad++;
		}
	}
	native_a630_gpu_gmu_fw_sum = fw_sum;
	native_a630_gpu_gmu_fw_bad = fw_bad;
	native_a630_gpu_gmu_fw_first_bad = fw_first;
	if (fw_bad) {
		failure = 39;
		goto restore;
	}
	printf("a630gpu: Stage 11 firmware remains loaded: %u words, hash %#010x\n",
	    A630_GMU_FW_WORDS, native_a630_gpu_gmu_fw_hash);

	for (uint32_t i = 0; i < A630_GMU_BOOT_REGS; i++) {
		uint32_t value = a630_gmu_boot_regs[i].value;
		uint32_t old = rd(gmu, GMU_WORD(a630_gmu_boot_regs[i].word));

		boot_old[i] = old;
		if (a630_gmu_boot_regs[i].word == GMU_RPMH_CTRL_W) {
			value |= old;
		}
		wr(gmu, GMU_WORD(a630_gmu_boot_regs[i].word), value);
		boot_written = i + 1;
		if (rd(gmu, GMU_WORD(a630_gmu_boot_regs[i].word)) != value) {
			native_a630_gpu_gmu_boot_index = i;
			native_a630_gpu_gmu_boot_old = old;
			native_a630_gpu_gmu_boot_want = value;
			native_a630_gpu_gmu_boot_got =
			    rd(gmu, GMU_WORD(a630_gmu_boot_regs[i].word));
			native_a630_gpu_gmu_boot_bad = 1;
			failure = 40;
			goto restore;
		}
	}
	native_a630_gpu_gmu_boot_count = boot_written;

	rsc_touched = true;
	if (!a630_gmu_boot_rsc_program(gmu, rsc_old, &rsc_old_enable,
	    &rsc_written, &rsc_bad)) {
		failure = 40;
		goto restore;
	}
	native_a630_gpu_gmu_init_result = 0;
	native_a630_gpu_gmu_cp_resp = 0;
	native_a630_gpu_gmu_hfi_status =
	    rd(gmu, GMU_WORD(GMU_HFI_CTRL_STATUS_W));

	old_ao_mask = rd(gmu, GMU_WORD(GMU_AO_HOST_MASK_W));
	old_gmu_mask = rd(gmu, GMU_WORD(GMU_GMU2HOST_MASK_W));
	wr(gmu, GMU_WORD(GMU_AO_HOST_MASK_W), ~0u);
	wr(gmu, GMU_WORD(GMU_GMU2HOST_MASK_W), ~0u);
	masks_changed = true;

	log = (uint8_t *)(block + 0x3000 + 5 * 0x1000);
	for (uint32_t i = 0; i < A630_GMU_HFI_BYTES / 4; i++) {
		((uint32_t *)log)[i] = 0;
	}
	a630_to_memory((vm_offset_t)log, A630_GMU_HFI_BYTES);
	wr(gmu, GMU_WORD(GMU_PWR_COL_CP_RESP_W), 0);
	wr(gmu, GMU_WORD(GMU_CM3_FW_INIT_RESULT_W), 0);
	wr(smmu, SMMU_CB_FSR, A630_SMMU_FSR_ALL);
	wr(smmu, SMMU_SGFSR, rd(smmu, SMMU_SGFSR));

	selector =
	    rd(gmu, GMU_WORD(GMU_CM3_DTCM_W) + 4u * 0xff8);
	if (selector <= 0x20010004u) {
		native_a630_gpu_gmu_init_mask = ~0u;
		native_a630_gpu_gmu_init_want = 0xbabefaceu;
	} else {
		native_a630_gpu_gmu_init_mask = 0x1ffu;
		native_a630_gpu_gmu_init_want = 0x100u;
	}
	printf("a630gpu: DTCM selector %#010x: waiting for FW_INIT_RESULT & %#x == %#x\n",
	    selector, native_a630_gpu_gmu_init_mask,
	    native_a630_gpu_gmu_init_want);

	printf("a630gpu: Stage 11 releasing CM3 reset with GX off\n");
	released = true;
	wr(gmu, GMU_WORD(GMU_CM3_SYSRESET_W), 0);
	for (uint32_t i = 0; i < 1000; i++) {
		result = rd(gmu, GMU_WORD(GMU_CM3_FW_INIT_RESULT_W));
		if ((result & native_a630_gpu_gmu_init_mask) ==
		    native_a630_gpu_gmu_init_want) {
			started = true;
			break;
		}
		sleep_us(100);
	}
	native_a630_gpu_gmu_init_result = result;
	native_a630_gpu_gmu_started = started;
	native_a630_gpu_gmu_cp_resp =
	    rd(gmu, GMU_WORD(GMU_PWR_COL_CP_RESP_W));
	native_a630_gpu_gmu_general7 =
	    rd(gmu, GMU_WORD(GMU_GENERAL_7_W));
	native_a630_gpu_gmu_sysreset =
	    rd(gmu, GMU_WORD(GMU_CM3_SYSRESET_W));
	native_a630_gpu_gmu_cm3_cfg = rd(gmu, GMU_WORD(GMU_CM3_CFG_W));
	native_a630_gpu_gmu_hfi_status =
	    rd(gmu, GMU_WORD(GMU_HFI_CTRL_STATUS_W));
	printf("a630gpu: GMU cold start %s: init %#010x want %#x/%#x CP_RESP %#010x GENERAL_7 %#x HFI %#010x\n",
	    started ? "DONE" : "TIMED OUT (100 ms)", result,
	    native_a630_gpu_gmu_init_want,
	    native_a630_gpu_gmu_init_mask,
	    native_a630_gpu_gmu_cp_resp, native_a630_gpu_gmu_general7,
	    native_a630_gpu_gmu_hfi_status);
	if (started && hfi_mode) {
		native_a630_gpu_gmu_hfi_started = 0;
		native_a630_gpu_hfi_stage = 1;
		old_hfi_init = rd(gmu, GMU_WORD(GMU_HFI_CTRL_INIT_W));
		wr(gmu, GMU_WORD(GMU_HFI_CTRL_INIT_W), 1);
		hfi_control_touched = true;
		for (uint32_t i = 0; i < 1000; i++) {
			if (rd(gmu, GMU_WORD(GMU_HFI_CTRL_STATUS_W)) & 1u) {
				native_a630_gpu_gmu_hfi_started = 1;
				break;
			}
			sleep_us(100);
		}
		native_a630_gpu_gmu_hfi_status =
		    rd(gmu, GMU_WORD(GMU_HFI_CTRL_STATUS_W));
		printf("a630gpu: HFI control channel %s: status %#010x\n",
		    native_a630_gpu_gmu_hfi_started ? "started" :
		    "TIMED OUT (100 ms)",
		    native_a630_gpu_gmu_hfi_status);
		if (!native_a630_gpu_gmu_hfi_started) {
			failure = 44;
		}
		if (!failure && hfi_mode >= 2) {
			if (!a630_hfi_handshake(gmu, block)) {
				failure = 45;
			}
		}
	}

restore:
	if (released) {
		wr(gmu, GMU_WORD(GMU_FENCE_CLR_W), 7);
		wr(gmu, GMU_WORD(GMU_AO_FENCE_CTRL_W), 0);
		__asm__ volatile("dsb sy" ::: "memory");
		wr(gmu, GMU_WORD(GMU_CM3_SYSRESET_W), 1);
		__asm__ volatile("dsb sy" ::: "memory");
		halted = false;
		for (uint32_t i = 0; i < 100; i++) {
			if (rd(gmu, GMU_WORD(GMU_CM3_SYSRESET_W)) == 1) {
				halted = true;
				break;
			}
			sleep_us(10);
		}
	}
	native_a630_gpu_gmu_sysreset =
	    rd(gmu, GMU_WORD(GMU_CM3_SYSRESET_W));
	if (!halted) {
		native_a630_gpu_status = 42;
		printf("a630gpu: CM3 did NOT return to reset; retaining all live resources\n");
		return false;
	}

	if (released && log != NULL) {
		a630_to_memory((vm_offset_t)log, A630_GMU_HFI_BYTES);
		for (uint32_t i = 0; i < 4; i++) {
			native_a630_gpu_gmu_log[i] = ((uint32_t *)log)[i];
		}
	}
	native_a630_gpu_smmu_sgfsr = rd(smmu, SMMU_SGFSR);
	native_a630_gpu_smmu_cb_fsr[A630_GMU_CTX] =
	    rd(smmu, native_a630_gpu_smmu_cb_base +
	    native_a630_gpu_smmu_page_size * A630_GMU_CTX + SMMU_CB_FSR);

	if (tcm_backed) {
		native_a630_gpu_gmu_tcm_boot_restore_bad =
		    a630_gmu_boot_tcm_restore(gmu);
		restore_bad += native_a630_gpu_gmu_tcm_boot_restore_bad;
		tcm_backed = false;
	}
	if (rsc_touched) {
		native_a630_gpu_gmu_rsc_boot_restore_bad =
		    a630_gmu_boot_rsc_restore(gmu, rsc_old, rsc_written,
		    rsc_old_enable);
		restore_bad += native_a630_gpu_gmu_rsc_boot_restore_bad;
		native_a630_gpu_rsc_restore_bad =
		    native_a630_gpu_gmu_rsc_boot_restore_bad;
		native_a630_gpu_rsc_final_enable =
		    rd(a630_pdc_base, 4u * 0x1140);
		rsc_touched = false;
	}
	if (boot_written) {
		native_a630_gpu_gmu_boot_restore_bad =
		    a630_gmu_boot_restore(gmu, boot_old, boot_written);
		restore_bad += native_a630_gpu_gmu_boot_restore_bad;
		native_a630_gpu_gmu_restore_bad =
		    native_a630_gpu_gmu_boot_restore_bad;
		boot_written = 0;
	}
	if (masks_changed) {
		wr(gmu, GMU_WORD(GMU_AO_HOST_MASK_W), old_ao_mask);
		wr(gmu, GMU_WORD(GMU_GMU2HOST_MASK_W), old_gmu_mask);
		masks_changed = false;
	}
	if (hfi_control_touched) {
		wr(gmu, GMU_WORD(GMU_HFI_CTRL_INIT_W), old_hfi_init);
		native_a630_gpu_gmu_hfi_restore_bad =
		    rd(gmu, GMU_WORD(GMU_HFI_CTRL_INIT_W)) == old_hfi_init ? 0 : 1;
		restore_bad += native_a630_gpu_gmu_hfi_restore_bad;
		hfi_control_touched = false;
	}
	if (mapped) {
		native_a630_gpu_gmu_smmu_boot_restore_bad =
		    a630_gmu_smmu_restore(smmu, block, &smmu_old);
		if (native_a630_gpu_gmu_smmu_boot_restore_bad == 0xffffffffu) {
			restore_bad++;
		} else {
			restore_bad +=
			    native_a630_gpu_gmu_smmu_boot_restore_bad;
			if (native_a630_gpu_gmu_smmu_boot_restore_bad == 0) {
				native_a630_gpu_smmu_gmu_block = 0;
				native_a630_gpu_smmu_gmu_root = 0;
				native_a630_gpu_smmu_gmu_pages = 0;
			}
		}
		mapped = false;
	}
	if (restore_bad) {
		native_a630_gpu_status = 43;
		printf("a630gpu: Stage 11 cleanup failed: %u mismatches\n",
		    restore_bad);
		return false;
	}
	if (failure) {
		native_a630_gpu_status = failure;
		return false;
	}
	if (!started) {
		native_a630_gpu_status = 41;
		return false;
	}
	native_a630_gpu_status = 0;
	printf("a630gpu: Stage 11 complete: firmware init acknowledged, CM3 held and all resources restored\n");
	return true;
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

		if (enabled < 5) {
			native_a630_gpu_status = 0;
			printf("a630gpu: stage 4 complete: GMU boot registers are writable; reset/firmware/SMMU untouched\n");
			return;
		}

		native_a630_gpu_stage = 11;
		if (!(native_a630_gpu_cx_gdscr & GDSC_PWR_ON) ||
		    (native_a630_gpu_memnoc & CBCR_OFF)) {
			native_a630_gpu_status = 17;
			printf("a630gpu: CX or MEMNOC is off; GPU SMMU is deliberately not mapped\n");
			return;
		}

		volatile uint8_t *smmu = (volatile uint8_t *)ml_io_map(SMMU_PHYS, SMMU_SIZE);
		if (smmu == NULL) {
			native_a630_gpu_status = 18;
			printf("a630gpu: GPU SMMU mapping failed\n");
			return;
		}

		native_a630_gpu_smmu_scr0 = rd(smmu, SMMU_SCR0);
		native_a630_gpu_smmu_sacr = rd(smmu, SMMU_SACR);
		native_a630_gpu_smmu_id0 = rd(smmu, SMMU_ID0);
		native_a630_gpu_smmu_id1 = rd(smmu, SMMU_ID1);
		native_a630_gpu_smmu_id2 = rd(smmu, SMMU_ID2);
		native_a630_gpu_smmu_id7 = rd(smmu, SMMU_ID7);
		native_a630_gpu_smmu_sgfsr = rd(smmu, SMMU_SGFSR);
		native_a630_gpu_smmu_sgfsynr0 = rd(smmu, SMMU_SGFSYNR0);
		native_a630_gpu_smmu_num_smr = native_a630_gpu_smmu_id0 & 0xff;
		native_a630_gpu_smmu_num_cb = native_a630_gpu_smmu_id1 & 0xff;
		native_a630_gpu_smmu_page_size =
		    (native_a630_gpu_smmu_id1 & (1u << 31)) ? 0x10000 : 0x1000;
		native_a630_gpu_smmu_cb_base =
		    (1u << (((native_a630_gpu_smmu_id1 >> 28) & 7) + 1)) *
		    native_a630_gpu_smmu_page_size;

		printf("a630gpu: GPU SMMU SCR0 %#010x SACR %#010x ID0 %#010x ID1 %#010x ID2 %#010x ID7 %#010x\n",
		    native_a630_gpu_smmu_scr0, native_a630_gpu_smmu_sacr,
		    native_a630_gpu_smmu_id0, native_a630_gpu_smmu_id1,
		    native_a630_gpu_smmu_id2, native_a630_gpu_smmu_id7);
		printf("a630gpu: GPU SMMU SMR %u CB %u page %#x CB base %#x fault %#010x/%#010x\n",
		    native_a630_gpu_smmu_num_smr, native_a630_gpu_smmu_num_cb,
		    native_a630_gpu_smmu_page_size, native_a630_gpu_smmu_cb_base,
		    native_a630_gpu_smmu_sgfsr, native_a630_gpu_smmu_sgfsynr0);

		uint64_t context_end = (uint64_t)native_a630_gpu_smmu_cb_base +
		    (uint64_t)native_a630_gpu_smmu_num_cb * native_a630_gpu_smmu_page_size;
		if (native_a630_gpu_smmu_cb_base >= SMMU_SIZE ||
		    context_end > SMMU_SIZE) {
			native_a630_gpu_status = 19;
			printf("a630gpu: GPU SMMU context layout is outside the mapped window: end %#llx\n",
			    context_end);
			return;
		}

		for (uint32_t i = 0; i < native_a630_gpu_smmu_num_smr &&
		    i < A630_SMMU_ROWS; i++) {
			native_a630_gpu_smmu_smr[i] = rd(smmu, SMMU_SMR(i));
			native_a630_gpu_smmu_s2cr[i] = rd(smmu, SMMU_S2CR(i));
			if ((native_a630_gpu_smmu_smr[i] & (1u << 31)) ||
			    native_a630_gpu_smmu_s2cr[i] != 0x000200ff) {
				printf("a630gpu: GPU SMMU route %u SMR %#010x S2CR %#010x\n",
				    i, native_a630_gpu_smmu_smr[i],
				    native_a630_gpu_smmu_s2cr[i]);
			}
		}

		native_a630_gpu_smmu_used_cb = 0;
		for (uint32_t i = 0; i < native_a630_gpu_smmu_num_cb &&
		    i < A630_SMMU_ROWS; i++) {
			uint32_t offset = native_a630_gpu_smmu_cb_base +
			    i * native_a630_gpu_smmu_page_size;

			native_a630_gpu_smmu_cbar[i] = rd(smmu, SMMU_CBAR(i));
			native_a630_gpu_smmu_cba2r[i] = rd(smmu, SMMU_CBA2R(i));
			native_a630_gpu_smmu_cb_sctlr[i] = rd(smmu, offset + SMMU_CB_SCTLR);
			native_a630_gpu_smmu_cb_ttbr0_lo[i] = rd(smmu, offset + SMMU_CB_TTBR0_LO);
			native_a630_gpu_smmu_cb_ttbr0_hi[i] = rd(smmu, offset + SMMU_CB_TTBR0_HI);
			native_a630_gpu_smmu_cb_tcr[i] = rd(smmu, offset + SMMU_CB_TCR);
			native_a630_gpu_smmu_cb_mair0[i] = rd(smmu, offset + SMMU_CB_MAIR0);
			native_a630_gpu_smmu_cb_fsr[i] = rd(smmu, offset + SMMU_CB_FSR);
			if (native_a630_gpu_smmu_cbar[i] ||
			    native_a630_gpu_smmu_cba2r[i] ||
			    native_a630_gpu_smmu_cb_sctlr[i]) {
				native_a630_gpu_smmu_used_cb++;
			}
			printf("a630gpu: GPU SMMU context %u CBAR %#010x CBA2R %#010x SCTLR %#010x TTBR0 %#010x%08x TCR %#010x MAIR0 %#010x FSR %#010x\n",
			    i, native_a630_gpu_smmu_cbar[i],
			    native_a630_gpu_smmu_cba2r[i],
			    native_a630_gpu_smmu_cb_sctlr[i],
			    native_a630_gpu_smmu_cb_ttbr0_hi[i],
			    native_a630_gpu_smmu_cb_ttbr0_lo[i],
			    native_a630_gpu_smmu_cb_tcr[i],
			    native_a630_gpu_smmu_cb_mair0[i],
			    native_a630_gpu_smmu_cb_fsr[i]);
		}
		native_a630_gpu_stage = 12;
		printf("a630gpu: GPU SMMU read-only qualification complete: %u used context banks\n",
		    native_a630_gpu_smmu_used_cb);

		if (enabled < 6) {
			native_a630_gpu_status = 0;
			printf("a630gpu: stage 5 complete: GPU SMMU is readable and the stream/context layout is safe\n");
			return;
		}
		if (!a630_gmu_smmu_stage6(smmu)) {
			return;
		}
		if (enabled < 7) {
			native_a630_gpu_status = 0;
			printf("a630gpu: stage 6 complete: GMU stream 5 translates through CB1 and the mapping was restored\n");
			return;
		}
		if (!a630_gmu_tcm_stage7(gmu)) {
			return;
		}
		if (enabled < 8) {
			native_a630_gpu_status = 0;
			printf("a630gpu: stage 7 complete: GMU TCM is writable while CM3 remains in reset\n");
			return;
		}
		if (!a630_gmu_rsc_pdc_stage8(gmu)) {
			return;
		}
		if (enabled < 9) {
			native_a630_gpu_status = 0;
			printf("a630gpu: stage 8 complete: A630 RSC/PDC state is readable while CM3 remains in reset\n");
			return;
		}
		if (!a630_rsc_pdc_write_stage9(gmu)) {
			return;
		}
		if (enabled < 10) {
			native_a630_gpu_status = 0;
			printf("a630gpu: stage 9 complete: A630 RSC/PDC table is writable and restorable\n");
			return;
		}
		if (!a630_gmu_firmware_stage10(gmu)) {
			return;
		}
		if (enabled < 11) {
			native_a630_gpu_status = 0;
			printf("a630gpu: stage 10 complete: the current A630 GMU firmware loads and restores without releasing CM3\n");
			return;
		}
		if (enabled < 13) {
			uint32_t hfi_mode = enabled >= 12 ? 1 : 0;

			if (!a630_gmu_boot_stage11(gmu, smmu, hfi_mode)) {
				return;
			}
			native_a630_gpu_status = 0;
			if (hfi_mode) {
				printf("a630gpu: stage 12 complete: GMU firmware and the HFI control channel start, then are held again\n");
			} else {
				printf("a630gpu: stage 11 complete: the A630 GMU cold-starts, reports firmware init, and is held again\n");
			}
			return;
		}
		if (!a630_gmu_boot_stage11(gmu, smmu, 2)) {
			return;
		}
	}

	native_a630_gpu_status = 0;
	printf("a630gpu: stage 13 complete: the complete A630 GMU HFI handshake is acknowledged and restored\n");
}
