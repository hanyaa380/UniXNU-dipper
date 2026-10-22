# ArmCpu

ARM64 CPU, GIC and timer driver for UniXNU. Publishes the CPU platform device, the interrupt controller and the architected timer.

Current state: starts the CPU, GIC and timer on sheng and on the QEMU virt machine.

Linked into XNU by `unixnu build kernel`.

## fajita copy

This copy of [UniXNU/ArmCpu](https://github.com/UniXNU/ArmCpu) at `1951cc8` overrides the base module for the OnePlus 6T. The only change is
the `armcpu-timer-intid` boot argument: SDM845 wires the EL1 virtual timer to PPI 3 (INTID 19) instead of PPI 27, so the board passes
`armcpu-timer-intid=19`. With 27 hardcoded no timer interrupt was ever taken and every timed wait (IOSleep) hung.
