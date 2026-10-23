# First real XNU run on dipper

The public fajita kernel's own prelink report marks it `loadable=false` and
`runtime_qualified=false`; it is only a bring-up candidate. On a standard Mi 8
it still reached much further than an early kernel crash.

## Boot path verified on hardware

DarwinBoot's top green band means that `ExitBootServices` completed and
`DarwinEnter()` ran in EL1, immediately before clearing the MMU and branching to
the kernel. It does not mean XNU has executed.

The temporary `patch-xnu-entry-probe.py` resets proved the following path:

| Probe | Result |
| --- | --- |
| XNU entry, immediately after `SCTLR_EL1`/MMU setup | reset |
| `_arm_init` and `_arm_vm_init` | reset |
| `_kernel_startup_initialize_upto` stages 4 and 12-16 | reset |
| `_machine_conf`, `_kernel_bootstrap` and `_sched_startup` | reset |
| `_kernel_bootstrap_thread` through `_IOKitBSDInit` | reset |
| `_IOFindBSDRoot` | no return after 50 seconds |

The screen showed three bands: DarwinBoot's handoff band, entry, and the MMU
setup probe. No panic appeared.

## Conclusion

XNU reaches BSD initialization and waits for an `IOMedia` root device. The
public fajita artifact does not contain `NativeUfs` or `native_ufs_*` symbols,
so it cannot expose dipper's UFS partition as a BSD root.

The next bootable kernel must be rebuilt from the current device configuration,
which links `NativeUfs`, storage bindings, boot progress and the display path.
It also needs a matching `userdata` layout; the board file deliberately has no
`[userdata]` geometry yet.

## Build inputs already available

- Linux-capable UniXNU reference checkout:
  `/root/mi8uefi/toolchain/UniXNU-fajita-linux`
- Apple `iig` replay inputs:
  `/root/mi8uefi/toolchain/unixnu-iig-outputs.zip`
- Public fajita kernel and compile records used for archive builders:
  `/root/mi8uefi/toolchain/fajita-kernel.zip`

The Linux kernel build additionally needs the custom clang and Apple `ld64`
built by the checkout's `scripts/build-xnu-clang-linux.sh` and
`scripts/build-ld64-linux.sh`.
