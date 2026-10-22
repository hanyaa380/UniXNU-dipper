# NativeDwc3

Native SDM845 DWC3 USB **device-mode** bring-up for the OnePlus 6T (fajita), the
kernel half of the SSH-over-USB channel (see `../../NOTES-ssh.md`).

Current state: **stage A, read-only probe only.** Default-off. Nothing is mapped or
read unless the boot argument `native-dwc3-probe=1` is present.

| boot arg | what it does |
| --- | --- |
| (none) | the driver attaches to the root device and does nothing |
| `native-dwc3-probe=1` | map GCC + the dwc3 core window UEFI left up, read a register snapshot (GSNPSID, GCTL, DCFG, DSTS, DALEPENA, event buffer) and the QUSB2 PHY PLL-ready bit. No MMIO is written. |
| `native-dwc3-probe=2` | also compute a **read-only** `apps_smmu` plan for the dwc3 stream `0x740`, the way NativeUfs plans the UFS stream, to prove the DMA path before any gadget code is written. |

Everything the probe sees is published through `native_dwc3_*` counters that
`package/BootLog.c` writes to the UFS diagnostic log (row prefix `U`), readable with
the sheng log reader over USB (`sheng-log=1`).

Hardware facts it relies on (all read-only):

- dwc3 core `0x0a600000` (window `0xcd00`), wrapper `0x0a6f8800`
  (`qcom,sdm845-dwc3` / `snps,dwc3`, device tree + Mu-Silicium `USB30_PRIM`).
- QUSB2 v2 HS PHY `0x088e2000` (`qcom,sdm845-qusb2-phy`).
- GCC `0x00100000`; the `USB30_PRIM` GDSC/branch offsets are from the SDM845 GCC
  driver. The probe only reads them to decide whether UEFI left the controller
  clocked, exactly as NativeUfs gates its UFS read.
- `apps_smmu` `0x15000000` (size `0x80000`); the dwc3 master is
  `iommus = <&apps_smmu 0x740 0>`.

Register offsets and field positions in `Dwc3Registers.h` / `Dwc3Phy.h` are
architectural facts of the Synopsys DWC3 IP and the Qualcomm QUSB2 PHY (the same
numbers mainline Linux records); no driver code is copied.

The CDC-ACM/-ECM gadget itself (event buffer, TRB rings, DEPCMDs, enumeration,
`ifnet`) is **stage B** and is not built yet. `NOTES-ssh.md` has the plan.

## Tests

`tests/run.sh` builds and runs host unit tests (`c++ -std=c++17`), like
`../NativeUfs/tests`:

- `Dwc3Decode.cpp` — the register decode helpers.
- `Dwc3Smmu.cpp` — the `apps_smmu` plan for the dwc3 stream `0x740`, reusing
  `../NativeUfs/SmmuTakeover.h`.

## Build

`scripts/build-fajita-dwc3.py` compiles the driver against this kernel's recorded
IOKit ABI and audits the Mach-O, producing `build/fajita-dwc3/libNativeDwc3.a`,
which `CONFIG_XNU_EXTRA_LD_ARCHIVES` force-loads into the kernel. The IOKit
personality is added by `patches/xnu/zzv-native-dwc3-personality.patch`.
