# NativeUfs

Native UFS storage driver. Owns the host controller, SMMU, queues and the root read path.

Current state: the native UFS root works on sheng.

Linked into XNU by `unixnu build kernel`.

## fajita copy

This copy of [UniXNU/NativeUfs](https://github.com/UniXNU/NativeUfs) at `af785c1` overrides the base module for the OnePlus 6T. The only change is the apps_smmu stream of the UFS host, which SDM845 maps with a mask (`iommus = <&apps_smmu 0x100 0xf>`) in a 0x80000 register window:

- `native-ufs-smmu-stream`, `native-ufs-smmu-mask` and `native-ufs-smmu-size` boot args; the defaults are sheng's values (0x60, 0, 0xd0000).
- The stream-match register carries the mask; an existing entry is reused only when it is exactly that value on the reserved bypass bank, and any other entry overlapping the stream's IDs is a conflict.
- `native-ufs-smmu-adopt=1` keeps the firmware's own entry for the stream when it is exactly that value on a bank with stage 1 translation off (fajita: slot 5 = 0x800f0100 on bank 2), without an SMMU write; off by default.
- `tests/SmmuTakeover.cpp` covers the SDM845 geometry, the masked stream and the adoption.
