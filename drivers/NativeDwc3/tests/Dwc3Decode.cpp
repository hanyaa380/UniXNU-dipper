// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for the DWC3 register decode helpers (Dwc3Registers.h / Dwc3Phy.h).
// Builds with c++ -std=c++17 on the host; no kernel headers (the headers use only
// <stdint.h>). Run by tests/run.sh.
#include <cassert>
#include <cstdio>
#include "../Dwc3Registers.h"
#include "../Dwc3Phy.h"

int main()
{
  // GSNPSID: DWC_usb3 reports 0x5533xxxx; the low 16 bits are the revision.
  assert(dwc3SnpsIsUsb3(0x5533330au));
  assert(dwc3SnpsClass(0x5533330au) == 0x5533);
  assert(dwc3SnpsRevision(0x5533330au) == 0x330a);
  assert(dwc3SnpsIsUsb3(0x33310200u));   // DWC_usb31
  assert(!dwc3SnpsIsUsb3(0x00000000u));
  assert(!dwc3SnpsIsUsb3(0xdeadbeefu));

  // GCTL current mode (prtcap) bits [13:12]: 2 = device.
  assert(dwc3GctlPrtCap(0x00002000u) == 2);   // device
  assert(dwc3GctlPrtCap(0x00001000u) == 1);   // host
  assert(dwc3GctlPrtCap(0x00003000u) == 3);   // OTG
  assert((kDwc3GctlCoreSoftReset) == (1u << 11));

  // DCFG: speed bits [2:0], device address bits [9:3].
  assert(dwc3DcfgSpeed(0x00000004u) == 4);          // super speed
  assert(dwc3DcfgSpeed(0x00000000u) == 0);          // high speed
  assert(dwc3DcfgDevAddr((5u << 3) | 0u) == 5);

  // DCTL run/stop bit31, core soft reset bit30.
  assert(kDwc3DctlRunStop == 0x80000000u);
  assert(kDwc3DctlCoreSoftReset == 0x40000000u);

  // DSTS: connected speed, link state [21:18], DEVCTRLHLT bit22.
  assert(dwc3DstsConnectSpeed(0x00000000u) == 0);
  assert(dwc3DstsLinkState((0x4u << 18)) == 0x4);
  assert(dwc3DstsHalted(1u << 22));
  assert(!dwc3DstsHalted(0));

  // Event buffer: size bits [15:0], mask bit31; count bits [15:0].
  assert(dwc3EvntSizBytes(0x80000100u) == 0x0100);
  assert(dwc3EvntSizMasked(0x80000100u));
  assert(!dwc3EvntSizMasked(0x00000100u));
  assert(dwc3EvntCountBytes(0x0000000cu) == 0x0c);

  // Per-endpoint command register strides (base + ep*0x10).
  assert(dwc3DepCmd(0) == 0xc80c);
  assert(dwc3DepCmd(1) == 0xc81c);
  assert(dwc3DepCmdPar0(2) == 0xc828);
  assert(dwc3DepCmdPar2(2) == 0xc820);

  // QUSB2 PHY ready bit.
  assert(qusb2PhyReady(kQusb2CoreReady));
  assert(!qusb2PhyReady(0));
  assert(kQusb2PllCommonStatusOne == 0x1a0);

  // GCC clock-gate decision mirrors NativeUfs: PWR_ON set and master/ahb2phy not CLK_OFF.
  Dwc3Snapshot s{};
  s.gcc_gdscr = 0x80000000u; s.gcc_master = 0x0; s.gcc_ahb2phy = 0x0;
  assert(dwc3GccCoreReadable(s));
  s.gcc_gdscr = 0x0;   // not powered
  assert(!dwc3GccCoreReadable(s));
  s.gcc_gdscr = 0x80000000u; s.gcc_master = 0x80000000u;   // master CLK_OFF
  assert(!dwc3GccCoreReadable(s));

  // SDM845 facts stay fixed.
  assert(kDwc3CoreBase == 0x0a600000ull);
  assert(kDwc3SmmuStream == 0x740);
  assert(kAppsSmmuBase == 0x15000000ull && kAppsSmmuSize == 0x80000ull);

  std::printf("Dwc3Decode: all register decode assertions passed\n");
  return 0;
}
