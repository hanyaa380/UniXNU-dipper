/* SPDX-License-Identifier: MIT
 * Kalama GCC + QSMMUv500 + DWC3. Values from the socrates live-DT and pinned
 * SM8550 sources listed in docs/sources-and-evidence.md. Inherited PHY/power only.
 */
#ifndef QUD_SM8550_H
#define QUD_SM8550_H
#define QUD_SOC_NAME "sm8550-inherited"
/* Development IDs inherited from m1n1, not a new product allocation. */
#define QUD_USB_VID 0x1209u
#define QUD_USB_PID 0x316du
#define QUD_USB_SERIAL "QcomXnuUsbDebug"
#define QUD_USB_PRODUCT "Qualcomm XNU Debug"
#define QUD_GCC_BASE 0x00100000ULL
#define QUD_GCC_BYTES 0x40000u
#define QUD_DWC3_BASE 0x0a600000ULL
#define QUD_DWC3_BYTES 0x100000u
#define QUD_SMMU_BASE 0x15000000ULL
#define QUD_SMMU_BYTES 0x100000ULL
#define QUD_STREAM_ID 0x40u
#define QUD_DMA_IOVA 0x90000000ULL
#define QUD_GCC_CBCR_OFFSETS {0x39018u, 0x39088u, 0x3908cu, 0x39028u, 0x39024u}
#define QUD_GCC_BCR_OFFSET 0x39000u
#define QUD_GCC_GDSCR_OFFSET 0x39004u
#define QUD_GCC_CFG_GDSCR_OFFSET 0x39008u
#define QUD_QSCRATCH_BASE QUD_DWC3_BASE
#define QUD_QSCRATCH_BYTES QUD_DWC3_BYTES
#define QUD_QSCRATCH_HS_PHY_CTRL_OFFSET 0xf8810u
#define QUD_QSCRATCH_VBUS_MASK ((1u << 20) | (1u << 28))
/* Only set after the board's PSCI SYSTEM_RESET through SMC is qualified. */
#define QUD_PSCI_SMC_RESET 1
#endif
