/* SPDX-License-Identifier: MIT */
#ifndef QCOM_USB_SOC_H
#define QCOM_USB_SOC_H
/* Pass -DQUD_SOC_HEADER='"soc/MySoc.h"', with the module root on -I.
 * No fallback guesses: a build must explicitly select its hardware profile.
 * The supplied backend requires clk_branch2/CFG_GDSCR, Qualcomm QSCRATCH,
 * non-secure QSMMUv500 SMMUv2 and inherited DEVICE-mode PHY/power.
 */
#ifndef QUD_SOC_HEADER
#error "Select hardware with -DQUD_SOC_HEADER=\"soc/Sm8550.h\""
#endif
#include QUD_SOC_HEADER
_Static_assert(QUD_PSCI_SMC_RESET == 0 || QUD_PSCI_SMC_RESET == 1,
               "declare whether the board qualifies PSCI reset via SMC");
_Static_assert(QUD_USB_VID > 0 && QUD_USB_VID <= 65535 &&
               QUD_USB_PID > 0 && QUD_USB_PID <= 65535, "USB IDs must fit 16 bits");
_Static_assert(QUD_STREAM_ID <= 0x7fffu, "legacy/extended SID must fit 15 bits");
_Static_assert(QUD_DMA_IOVA > 0 && QUD_DMA_IOVA <= 0xffe00000ULL &&
               (QUD_DMA_IOVA & 0x1fffffULL) == 0,
               "private three-page table builder needs a 2MiB-aligned 32-bit IOVA");
_Static_assert(QUD_GCC_BASE != 0 && QUD_DWC3_BASE != 0 && QUD_SMMU_BASE != 0 &&
               QUD_QSCRATCH_BASE != 0 &&
               ((QUD_GCC_BASE | QUD_DWC3_BASE | QUD_QSCRATCH_BASE | QUD_SMMU_BASE) & 4095u) == 0,
               "MMIO bases must be nonzero and page aligned");
_Static_assert(QUD_GCC_BYTES >= 4 && QUD_DWC3_BYTES >= 0xc800u &&
               QUD_QSCRATCH_BYTES >= 4u && QUD_SMMU_BYTES >= 0x10000u,
               "MMIO windows too small");
_Static_assert(QUD_GCC_BASE + QUD_GCC_BYTES > QUD_GCC_BASE &&
               QUD_DWC3_BASE + QUD_DWC3_BYTES > QUD_DWC3_BASE &&
               QUD_QSCRATCH_BASE + QUD_QSCRATCH_BYTES > QUD_QSCRATCH_BASE &&
               QUD_SMMU_BASE + QUD_SMMU_BYTES > QUD_SMMU_BASE,
               "MMIO window overflow");
#define QUD_DISJOINT(a, n, b, m) ((a) + (n) <= (b) || (b) + (m) <= (a))
_Static_assert(QUD_DISJOINT(QUD_GCC_BASE, QUD_GCC_BYTES, QUD_DWC3_BASE, QUD_DWC3_BYTES) &&
               QUD_DISJOINT(QUD_GCC_BASE, QUD_GCC_BYTES, QUD_SMMU_BASE, QUD_SMMU_BYTES) &&
               QUD_DISJOINT(QUD_DWC3_BASE, QUD_DWC3_BYTES, QUD_SMMU_BASE, QUD_SMMU_BYTES) &&
               (QUD_QSCRATCH_BASE == QUD_DWC3_BASE ||
                (QUD_DISJOINT(QUD_QSCRATCH_BASE, QUD_QSCRATCH_BYTES, QUD_GCC_BASE, QUD_GCC_BYTES) &&
                 QUD_DISJOINT(QUD_QSCRATCH_BASE, QUD_QSCRATCH_BYTES, QUD_DWC3_BASE, QUD_DWC3_BYTES) &&
                 QUD_DISJOINT(QUD_QSCRATCH_BASE, QUD_QSCRATCH_BYTES, QUD_SMMU_BASE, QUD_SMMU_BYTES))),
               "MMIO windows overlap");
#define QUD_OFFSET_OK(o, n) (((o) & 3u) == 0 && (o) <= (n) - 4)
_Static_assert(QUD_OFFSET_OK(QUD_GCC_BCR_OFFSET, QUD_GCC_BYTES) &&
               QUD_OFFSET_OK(QUD_GCC_GDSCR_OFFSET, QUD_GCC_BYTES) &&
               QUD_OFFSET_OK(QUD_GCC_CFG_GDSCR_OFFSET, QUD_GCC_BYTES) &&
               QUD_OFFSET_OK(QUD_QSCRATCH_HS_PHY_CTRL_OFFSET, QUD_QSCRATCH_BYTES),
               "platform register outside MMIO window");
#endif
