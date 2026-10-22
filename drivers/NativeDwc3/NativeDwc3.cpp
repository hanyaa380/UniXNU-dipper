// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native SDM845 DWC3 USB device-mode bring-up for the OnePlus 6T (fajita).
//
// Default-off: no MMIO is touched unless native-dwc3-probe=1 is present. This is a
// read-only register probe, staged exactly like NativeUfs (drivers/NativeUfs):
//   native-dwc3-probe=1  map GCC + the dwc3 core window UEFI left up, read a
//                        snapshot (GSNPSID/GCTL/DCFG/DSTS/DALEPENA/event buffer)
//                        and the QUSB2 PHY PLL-ready bit. Nothing is written.
//   native-dwc3-probe=2  also compute (read-only) the apps_smmu plan for the dwc3
//                        stream 0x740, the way NativeUfs plans the UFS stream, to
//                        prove the DMA path before any gadget code runs.
// Every stage advances a counter that package/BootLog.c surfaces on the UFS log.
// The CDC-ACM gadget itself (TRB ring, DEPCMDs, enumeration) is stage B and is NOT
// built here; see NOTES-ssh.md. This driver never writes a device-mode register.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSSharedPtr.h>
#include <pexpert/pexpert.h>
#include <kern/debug.h>
#define NATIVE_DWC3_KERNEL 1
#include "Dwc3Registers.h"
#include "Dwc3Phy.h"
#include "../NativeUfs/SmmuTakeover.h"   // generic ARM SMMUv2 stream planner (read-only use here)

extern "C" {
// Counters the boot log reads (package/BootLog.c), all weak so a board without the
// log still links. Mirrors NativeUfs's native_ufs_* counter block.
volatile uint32_t native_dwc3_probe_calls = 0;
volatile uint32_t native_dwc3_matched = 0;
volatile uint32_t native_dwc3_mode = 0;      // native-dwc3-probe value actually seen
volatile uint32_t native_dwc3_stage = 0;     // 1 gcc, 2 core snapshot, 3 phy, 4 smmu plan, 5 done
volatile int32_t  native_dwc3_status = -1;    // 0 ok; negative = stage that soft-failed
// Snapshot, for the log.
volatile uint32_t native_dwc3_gsnpsid = 0, native_dwc3_gctl = 0, native_dwc3_gsts = 0;
volatile uint32_t native_dwc3_dcfg = 0, native_dwc3_dctl = 0, native_dwc3_dsts = 0;
volatile uint32_t native_dwc3_dalepena = 0, native_dwc3_devten = 0;
volatile uint32_t native_dwc3_ghwparams0 = 0, native_dwc3_ghwparams1 = 0, native_dwc3_ghwparams3 = 0;
volatile uint32_t native_dwc3_gevntsiz0 = 0, native_dwc3_gevntcount0 = 0;
volatile uint32_t native_dwc3_qusb2_status = 0, native_dwc3_phy_ready = 0;
volatile uint32_t native_dwc3_gcc_gdscr = 0, native_dwc3_gcc_master = 0, native_dwc3_gcc_mock = 0;
volatile uint32_t native_dwc3_gcc_ahb2phy = 0, native_dwc3_gcc_noc_axi = 0, native_dwc3_gcc_aggre_axi = 0;
volatile uint32_t native_dwc3_prtcap = 0;    // GCTL current mode (2 = device)
volatile int32_t  native_dwc3_smmu_slot = -1, native_dwc3_smmu_bank = -1;
volatile uint32_t native_dwc3_smmu_reuse = 0;
const char *volatile native_dwc3_smmu_error = nullptr;
const char *volatile native_dwc3_failure = nullptr;
}

static bool gProbeEnabled = false;
static unsigned gProbeMode = 0;

static uint32_t dwc3Read32(uintptr_t base, unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *>(base + offset);
}

static uintptr_t dwc3MapPhys(uint64_t phys, uint64_t length, OSSharedPtr<IOMemoryMap> &keep)
{
  IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
  if (!memory) {
    return 0;
  }
  keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  memory->release();
  return keep ? static_cast<uintptr_t>(keep->getVirtualAddress()) : 0;
}

class NativeDwc3 : public IOService {
  OSDeclareDefaultStructors(NativeDwc3)
  void inspect();
public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(NativeDwc3, IOService)

IOService *NativeDwc3::probe(IOService *provider, SInt32 *score)
{
  native_dwc3_probe_calls = native_dwc3_probe_calls + 1;
  // Attach to the root device, like NativeUfs's second personality: it starts
  // before the platform expert publishes IOResources, so the probe runs early and
  // before the original AMFI. The probe is a no-op unless the boot arg opts in.
  if (OSDynamicCast(IOPlatformExpertDevice, provider)) {
    native_dwc3_matched = 1;
    if (score) {
      *score = 500;
    }
    return this;
  }
  return nullptr;
}

bool NativeDwc3::start(IOService *provider)
{
  if (!IOService::start(provider)) {
    return false;
  }
  uint32_t value = 0;
  if (PE_parse_boot_argn("native-dwc3-probe", &value, sizeof(value))) {
    gProbeMode = value;
    gProbeEnabled = value != 0;
  }
  native_dwc3_mode = gProbeMode;
  if (!gProbeEnabled) {
    return true;   // default off: nothing is mapped or read
  }
  inspect();
  return true;
}

// Read-only inspection. Each stage sets native_dwc3_stage before it touches MMIO,
// so a fault leaves the last safe stage recorded in the log. Nothing is written.
void NativeDwc3::inspect()
{
  OSSharedPtr<IOMemoryMap> gccMap, coreMap, phyMap, smmuMap;
  Dwc3Snapshot snap;
  bzero(&snap, sizeof(snap));

  native_dwc3_stage = 1;
  uintptr_t gcc = dwc3MapPhys(kGccBase, 0x100000, gccMap);
  if (!gcc) {
    native_dwc3_failure = "gcc map";
    native_dwc3_status = -1;
    return;
  }
  snap.gcc_gdscr    = dwc3Read32(gcc, kGccUsb30PrimGdscr);
  snap.gcc_master   = dwc3Read32(gcc, kGccUsb30PrimMasterCbcr);
  snap.gcc_mock_utmi = dwc3Read32(gcc, kGccUsb30PrimMockUtmiCbcr);
  snap.gcc_sleep    = dwc3Read32(gcc, kGccUsb30PrimSleepCbcr);
  snap.gcc_noc_axi  = dwc3Read32(gcc, kGccCfgNocUsb3PrimCbcr);
  snap.gcc_aggre_axi = dwc3Read32(gcc, kGccAggreUsb3PrimCbcr);
  snap.gcc_ahb2phy  = dwc3Read32(gcc, kGccUsbPhyCfgAhb2PhyCbcr);
  native_dwc3_gcc_gdscr = snap.gcc_gdscr;
  native_dwc3_gcc_master = snap.gcc_master;
  native_dwc3_gcc_mock = snap.gcc_mock_utmi;
  native_dwc3_gcc_ahb2phy = snap.gcc_ahb2phy;
  native_dwc3_gcc_noc_axi = snap.gcc_noc_axi;
  native_dwc3_gcc_aggre_axi = snap.gcc_aggre_axi;

  if (!dwc3GccCoreReadable(snap)) {
    // UEFI did not leave the controller clocked: report and stop, do not guess.
    native_dwc3_failure = "usb30 clocks off after hand-off";
    native_dwc3_status = -1;
    return;
  }

  native_dwc3_stage = 2;
  uintptr_t core = dwc3MapPhys(kDwc3CoreBase, kDwc3CoreSize, coreMap);
  if (!core) {
    native_dwc3_failure = "core map";
    native_dwc3_status = -2;
    return;
  }
  snap.gsnpsid    = dwc3Read32(core, kDwc3GSNPSID);
  snap.gctl       = dwc3Read32(core, kDwc3GCTL);
  snap.gsts       = dwc3Read32(core, kDwc3GSTS);
  snap.ghwparams0 = dwc3Read32(core, kDwc3GHWPARAMS0);
  snap.ghwparams1 = dwc3Read32(core, kDwc3GHWPARAMS1);
  snap.ghwparams3 = dwc3Read32(core, kDwc3GHWPARAMS3);
  snap.dcfg       = dwc3Read32(core, kDwc3DCFG);
  snap.dctl       = dwc3Read32(core, kDwc3DCTL);
  snap.dsts       = dwc3Read32(core, kDwc3DSTS);
  snap.dalepena   = dwc3Read32(core, kDwc3DALEPENA);
  snap.devten     = dwc3Read32(core, kDwc3DEVTEN);
  snap.gevntsiz0  = dwc3Read32(core, kDwc3GEVNTSIZ0);
  snap.gevntcount0 = dwc3Read32(core, kDwc3GEVNTCOUNT0);
  snap.gusb2phycfg0 = dwc3Read32(core, kDwc3GUSB2PHYCFG0);
  native_dwc3_gsnpsid = snap.gsnpsid;
  native_dwc3_gctl = snap.gctl;
  native_dwc3_gsts = snap.gsts;
  native_dwc3_ghwparams0 = snap.ghwparams0;
  native_dwc3_ghwparams1 = snap.ghwparams1;
  native_dwc3_ghwparams3 = snap.ghwparams3;
  native_dwc3_dcfg = snap.dcfg;
  native_dwc3_dctl = snap.dctl;
  native_dwc3_dsts = snap.dsts;
  native_dwc3_dalepena = snap.dalepena;
  native_dwc3_devten = snap.devten;
  native_dwc3_gevntsiz0 = snap.gevntsiz0;
  native_dwc3_gevntcount0 = snap.gevntcount0;
  native_dwc3_prtcap = dwc3GctlPrtCap(snap.gctl);
  if (!dwc3SnpsIsUsb3(snap.gsnpsid)) {
    native_dwc3_failure = "unexpected GSNPSID (not a DWC3 core)";
    native_dwc3_status = -2;
    return;
  }

  native_dwc3_stage = 3;
  uintptr_t phy = dwc3MapPhys(kQusb2PhyBase, kQusb2PhySize, phyMap);
  if (phy) {
    snap.qusb2_pll_status = dwc3Read32(phy, kQusb2PllCommonStatusOne);
    native_dwc3_qusb2_status = snap.qusb2_pll_status;
    native_dwc3_phy_ready = qusb2PhyReady(snap.qusb2_pll_status) ? 1 : 0;
  } else {
    native_dwc3_qusb2_status = 0;
    native_dwc3_phy_ready = 0;
  }

  if (gProbeMode >= 2) {
    native_dwc3_stage = 4;
    // Read-only SMMU plan for the dwc3 stream, exactly as NativeUfs plans the UFS
    // stream, but nothing is configured. A representative 64 KiB DMA window at a
    // plausible address only exercises the planner's address-width checks; the
    // real gadget would pass its own event-buffer/TRB allocation here.
    uintptr_t smmu = dwc3MapPhys(kAppsSmmuBase, kAppsSmmuSize, smmuMap);
    if (!smmu) {
      native_dwc3_failure = "smmu map";
      native_dwc3_status = -4;
      return;
    }
    const uint64_t probeDma = 0x80000000ull, probeBytes = 0x10000ull;
    UfsSmmuPlan plan = planUfsSmmu(kDwc3SmmuStream, kDwc3SmmuMask, kAppsSmmuSize, probeDma, probeBytes,
                                   [smmu](unsigned off) { return dwc3Read32(smmu, off); }, false);
    native_dwc3_smmu_slot = plan.error ? -1 : int32_t(plan.slot);
    native_dwc3_smmu_bank = plan.error ? -1 : int32_t(plan.bank);
    native_dwc3_smmu_reuse = plan.reuse ? 1 : 0;
    native_dwc3_smmu_error = plan.error;
  }

  native_dwc3_stage = 5;
  native_dwc3_status = 0;
  native_dwc3_failure = nullptr;
}
