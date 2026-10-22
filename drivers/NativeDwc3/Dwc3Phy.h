// SPDX-License-Identifier: BSD-2-Clause-Patent
// QUSB2 v2 high-speed PHY (qcom,sdm845-qusb2-phy @ 0x088e2000) register offsets
// used by the read-only probe. Offsets are facts of the Qualcomm QUSB2 v2 PHY,
// as recorded in the mainline driver drivers/phy/qualcomm/phy-qcom-qusb2.c
// (GPL-2.0); no PHY code is copied, only the status offset and its ready bit.
#ifndef NATIVE_DWC3_PHY_H
#define NATIVE_DWC3_PHY_H

#include <stdint.h>

enum : uint32_t {
  // PLL_COMMON_STATUS_ONE: bit0 CORE_READY_STATUS is set once the PHY PLL has
  // locked. UEFI's UsbfnDwc3Dxe brings the PHY up for fastboot, so a probe that
  // runs right after hand-off expects this set.
  kQusb2PllCommonStatusOne = 0x1a0,
  kQusb2CoreReady = 1u << 0,
};

static inline bool qusb2PhyReady(uint32_t status) { return (status & kQusb2CoreReady) != 0; }

#endif
