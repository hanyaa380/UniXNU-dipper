/* SPDX-License-Identifier: MIT
 * Xiaomi Mi 8 (dipper) SDM845 profile.
 *
 * Sources:
 * - Linux arch/arm64/boot/dts/qcom/sdm845.dtsi:
 *   usb_1 wrapper at 0x0a6f8800, DWC3 core at 0x0a600000, apps_smmu SID 0x740.
 * - Linux drivers/clk/qcom/gcc-sdm845.c:
 *   USB30_PRIM BCR/GDSCR and the five usb_1 clock branch registers.
 * - Linux drivers/usb/dwc3/dwc3-qcom.c:
 *   QSCRATCH_HS_PHY_CTRL offset 0x10 and VBUS/session-valid bits 20 and 28.
 * - This repository's read-only NativeDwc3 probe used on the inherited SDM845
 *   firmware state.
 */
#ifndef QUD_SM845_H
#define QUD_SM845_H

#define QUD_SOC_NAME "sdm845-dipper"

/* Development IDs inherited from m1n1, not a new product allocation. */
#define QUD_USB_VID 0x1209u
#define QUD_USB_PID 0x316du
#define QUD_USB_SERIAL "QcomXnuUsbDebug"
#define QUD_USB_PRODUCT "Qualcomm XNU Debug"

#define QUD_GCC_BASE 0x00100000ULL
#define QUD_GCC_BYTES 0x100000u
#define QUD_DWC3_BASE 0x0a600000ULL
#define QUD_DWC3_BYTES 0x10000u
/* Page-aligned mapping of the 0x0a6f8800 wrapper window. */
#define QUD_QSCRATCH_BASE 0x0a6f8000ULL
#define QUD_QSCRATCH_BYTES 0x1000u
#define QUD_SMMU_BASE 0x15000000ULL
#define QUD_SMMU_BYTES 0x80000ULL
#define QUD_STREAM_ID 0x740u
#define QUD_DMA_IOVA 0x90000000ULL

/* usb_1 clocks: core, cfg_noc, aggre, mock_utmi, sleep. */
#define QUD_GCC_CBCR_OFFSETS {0xf00cu, 0x502cu, 0x8201cu, 0xf014u, 0xf010u}
#define QUD_GCC_BCR_OFFSET 0xf000u
#define QUD_GCC_GDSCR_OFFSET 0xf004u
#define QUD_GCC_CFG_GDSCR_OFFSET 0xf008u

#define QUD_QSCRATCH_HS_PHY_CTRL_OFFSET 0x810u
#define QUD_QSCRATCH_VBUS_MASK ((1u << 20) | (1u << 28))

/* The SDM845 PSCI SYSTEM_RESET contract has not been qualified here yet. */
#define QUD_PSCI_SMC_RESET 0

#endif
