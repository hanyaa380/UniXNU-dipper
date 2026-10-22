# NativeRamBlock

RAM block storage driver. Provides block devices from RAM and a persistent Data overlay.

Current state: used for the RAM-backed Data volume and the optional persistent overlay.

Linked into XNU by `unixnu build kernel`.

## fajita copy

This is a copy of upstream UniXNU NativeRamBlock at d4d22d9 (the build links the device repo's `drivers/` ahead of upstream). fajita
adds an opt-in write-back mode, `native-data-writeback=1`: Data writes reach the store without FUA, the overlay's bitmap blocks are
committed by `PersistOverlay::flush` (cache flush, bitmap blocks, cache flush) when APFS synchronizes the cache and every 2 s while
writes are pending, and the device reports its write cache as enabled. Without the argument it behaves as upstream (write-through).
