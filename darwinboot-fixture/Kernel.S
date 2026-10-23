// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Hardware-only DarwinBoot handoff fixture.
//
// DarwinBoot calls this entry after exiting boot services, with the MMU and
// caches off. A valid handoff fills the framebuffer green; a rejected ABI fills
// it red. It never returns, so the result remains visible for a photograph.

.section .text, "ax"
.global _start
_start:
  cbz x0, fail
  ldr w1, [x0]
  mov w2, #0x20002
  cmp w1, w2
  b.ne fail

  // DarwinEnter cleared MMU and D/I cache enable bits in SCTLR_EL1.
  mrs x1, sctlr_el1
  tst x1, #1
  b.ne fail

  ldr x4, [x0, #0x28]       // framebuffer physical address
  ldr x5, [x0, #0x30]       // row bytes
  ldr x6, [x0, #0x38]       // width
  ldr x7, [x0, #0x40]       // height
  ldr x8, [x0, #0x48]       // bits per pixel
  cbz x4, fail
  cbz x5, fail
  cbz x6, fail
  cbz x7, fail
  cmp x8, #32
  b.ne fail
  lsl x9, x6, #2
  cmp x5, x9
  b.lo fail

  mov x10, x5
  mul x10, x10, x7
  lsr x10, x10, #2
  mov w11, #0xFF00
  movk w11, #0xFF00, lsl #16
  mov x12, x4
1:
  str w11, [x12], #4
  subs x10, x10, #1
  b.ne 1b
  dsb sy
  b hang

fail:
  // Use the boot_args-derived framebuffer when possible. If boot_args itself
  // is unusable there is no safe display output, so only spin.
  cbz x0, hang
  ldr x4, [x0, #0x28]
  ldr x5, [x0, #0x30]
  ldr x7, [x0, #0x40]
  cbz x4, hang
  cbz x5, hang
  cbz x7, hang
  mul x10, x5, x7
  lsr x10, x10, #2
  mov w11, #0x0000
  movk w11, #0xFF00, lsl #16
  mov x12, x4
2:
  str w11, [x12], #4
  subs x10, x10, #1
  b.ne 2b
  dsb sy

hang:
  wfe
  b hang
