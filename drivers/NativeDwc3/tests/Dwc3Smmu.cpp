// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test: the read-only apps_smmu plan the DWC3 probe (native-dwc3-probe=2)
// computes for the dwc3 stream, reusing the generic ARM SMMUv2 planner that
// drivers/NativeUfs/SmmuTakeover.h provides and NativeUfs's own tests cover. This
// test pins the DWC3 stream id (0x740, mask 0) and window (apps_smmu 0x80000) so a
// regression in either the planner or the DWC3 constants is caught offline.
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <array>
#include "../../NativeUfs/SmmuTakeover.h"
#include "../Dwc3Registers.h"

int main()
{
  // A self-consistent apps_smmu register image for the planner: IDR0 (0x20) 128
  // stream-match groups, IDR1 (0x24) 64 context banks and a 64-page global address
  // space (so (pages + banks) * 4KiB == the 0x80000 window), IDR2 (0x28) 36-bit
  // addressing, AArch64 stage-1 supported. These are a representative image chosen
  // to fit the real 0x80000 apps_smmu window, not SDM845's measured IDR words; the
  // point is to exercise the generic planner against the DWC3 stream id and window.
  std::array<uint32_t, 0x80000 / 4> r{};
  auto reset = [&] { r.fill(0); r[0x20 / 4] = 0x40000080; r[0x24 / 4] = 0x50000040; r[0x28 / 4] = 0x5111; };
  auto plan = [&] {
    return planUfsSmmu(kDwc3SmmuStream, kDwc3SmmuMask, kAppsSmmuSize, 0x80000000ull, 0x10000ull,
                       [&](unsigned at) { return r.at(at / 4); });
  };

  // A clean SMMU: the dwc3 stream gets the top free context bank and a free group.
  reset();
  UfsSmmuPlan p = plan();
  assert(!p.error && !p.reuse && p.slot == 127 && p.bank == 63);

  // Our own stream already mapped (a warm reset left it): reused, not re-planned.
  reset();
  r[(0x800 + 4 * 110) / 4] = ufsSmmuSmr(kDwc3SmmuStream, kDwc3SmmuMask);   // 0x80000740
  r[(0xc00 + 4 * 110) / 4] = 63;
  r[(0x1000 + 4 * 63) / 4] = 0x10000;
  p = plan();
  assert(!p.error && p.reuse && p.slot == 110);

  // The SMR we would program must carry the exact stream id with no mask.
  assert(ufsSmmuSmr(0x740, 0) == 0x80000740u);

  // Another stream squatting on the top bank: the plan falls to the next bank down.
  reset();
  r[0x800 / 4] = 0x80000061; r[0xc00 / 4] = 63;
  p = plan();
  assert(!p.error && !p.reuse && p.bank == 62 && p.slot == 127);

  // A conflicting mapping of our own stream on an enabled-translation bank is refused.
  reset();
  r[(0x800 + 4 * 5) / 4] = ufsSmmuSmr(kDwc3SmmuStream, kDwc3SmmuMask);
  r[(0xc00 + 4 * 5) / 4] = 1;   // points at bank 1, not a reserved bypass bank
  assert(plan().error);

  std::printf("Dwc3Smmu: stream 0x740 SMMU plan assertions passed\n");
  return 0;
}
