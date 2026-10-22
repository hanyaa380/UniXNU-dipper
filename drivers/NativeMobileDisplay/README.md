# NativeMobileDisplay

Mobile display driver and GPU scanout hand-off. Validates swaps, retires frames and drives the firmware framebuffer.

Current state: the CPU and GPU scanout paths work. The GOP scanout has no vblank interrupt; with `sheng-vsync=<Hz>` a work-loop timer sends the original family's vsync message to the compositor.

Linked into XNU by `unixnu build kernel`.

## fajita copy

This copy of [UniXNU/NativeMobileDisplay](https://github.com/UniXNU/NativeMobileDisplay) at `c031d88` overrides the base module for the
OnePlus 6T. The only change is a logical display size for NativeLegacyDisplay (`NativeLegacyDisplay.cpp`, `NativeLegacyScale.h`,
`FajitaDisplayLogical.h`):

- `fajita-display-logical=<W>x<H>` (off by default): the original family's dimension fields hold W x H, so every size userland sees
  (IOMFB properties, modes, swap and surface validation) is the logical one, while the scanout stays the UEFI GOP panel (1080x2340).
  iOS needs a screen size MobileGestalt knows for the device identity: iPad7,1 is 2048x2732 at @2x, so fajita passes
  `fajita-display-logical=2048x2732` with `offline-legacy-unit-scale=2` (`/chosen/display-scale` 2, 1024x1366 points).
- Each frame is fitted onto the panel (2048x2732 -> 1080x1441 at y=449), centered, with a black letterbox written once. Every image pixel
  is a 2x2 box average (2 taps per axis, precomputed per column and row) of the source; `sheng-fb-diff=1` stores only the 64-byte
  blocks that changed against a shadow of the scaled image. The frame statistics still describe the logical source. The GPU hand-off
  (`sheng-fb-gpu=1`) is not used for a logical display.
- `fajita-display-ppi=<N>` (off by default): answers the dot pitch query with N when the provider has no `dot-pitch` (iPad7,1: 264).
- `FajitaDisplayLogical.h` exports the logical size and the image rectangle on the panel (`fajita_display_logical`) with the
  panel-to-logical transform for touch.
