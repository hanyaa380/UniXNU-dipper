/* SPDX-License-Identifier: MIT
 * Host-side safety tests: no modelled register can write outside USB S2CR.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "soc/QcomUsbQualify.h"

struct reg { QUSB_U64 address; QUSB_U32 value; };
struct model {
  struct reg regs[64];
  unsigned int count, writes, barriers, dwc_reads, smr_reads;
  unsigned int cr0_reads, s2cr_reads, flip_cr0_read, flip_s2cr_read;
  QUSB_U32 replace_cr0, replace_s2cr;
  int fail_write;
};
static void set(struct model *m, QUSB_U64 address, QUSB_U32 value)
{
  unsigned int i;
  for (i = 0; i < m->count; ++i) if (m->regs[i].address == address) {
    m->regs[i].value = value; return;
  }
  assert(m->count < 64);
  m->regs[m->count++] = (struct reg){address, value};
}
static int read32(void *cookie, QUSB_U64 address, QUSB_U32 *value)
{
  struct model *m = cookie;
  unsigned int i;
  assert(!(address & 3U));
  assert((address >= QUSB_SMMU_BASE && address < QUSB_SMMU_BASE + QUSB_SMMU_BYTES) ||
         (address >= QUSB_DWC3_BASE && address < QUSB_DWC3_BASE + 0x10000) ||
         (address >= QUSB_GCC_BASE && address < QUSB_GCC_BASE + 0x1f4200));
  if (address >= QUSB_DWC3_BASE && address < QUSB_DWC3_BASE + 0x10000) ++m->dwc_reads;
  if (address >= QUSB_SMMU_BASE + 0x800 && address < QUSB_SMMU_BASE + 0xc00) ++m->smr_reads;
  if (address == QUSB_SMMU_BASE && ++m->cr0_reads == m->flip_cr0_read)
    set(m, address, m->replace_cr0);
  if (address == QUSB_SMMU_BASE + 0xc04 && ++m->s2cr_reads == m->flip_s2cr_read)
    set(m, address, m->replace_s2cr);
  for (i = 0; i < m->count; ++i) if (m->regs[i].address == address) {
    *value = m->regs[i].value; return 1;
  }
  *value = 0; return 1;
}
static int write32(void *cookie, QUSB_U64 address, QUSB_U32 value)
{
  struct model *m = cookie;
  assert(address == QUSB_SMMU_BASE + 0xc04);
  assert(value == 0x03000101U || value == 0x03010101U ||
         value == 0x03000501U || value == 0x03010501U);
  ++m->writes;
  if (m->fail_write) return 0;
  set(m, address, value); return 1;
}
static void barrier(void *cookie) { ++((struct model *)cookie)->barriers; }
static void init(struct model *m)
{
  memset(m, 0, sizeof(*m));
  set(m, QUSB_GCC_BASE + 0x39008, 1U << 16);
  set(m, QUSB_DWC3_BASE + 0xc120, 0x5533330aU);
  set(m, QUSB_DWC3_BASE + 0xc110, 2U << 12);
  set(m, QUSB_DWC3_BASE + 0xc70c, 1U << 22);
  set(m, QUSB_SMMU_BASE + 0x20, (1U << 30) | (1U << 27) | 3U);
  set(m, QUSB_SMMU_BASE + 0x24, 0x10000008U);
  set(m, QUSB_SMMU_BASE + 0x3c, 0x20U);
  set(m, QUSB_SMMU_BASE + 0x804, 0x80000040U);
  set(m, QUSB_SMMU_BASE + 0xc04, 0x03000101U);
}
static void init_extended(struct model *m)
{
  init(m);
  set(m, QUSB_SMMU_BASE + 0x20, (1U << 30) | (1U << 27) | (1U << 8) | 3U);
  set(m, QUSB_SMMU_BASE, (1U << 3) | (1U << 10));
  set(m, QUSB_SMMU_BASE + 0x804, 0x40U);
  set(m, QUSB_SMMU_BASE + 0xc04, 0x03000501U);
}
int main(void)
{
  struct model m;
  QUSB_SMMU_STATE s;
  QUSB_INHERITED hw;
  QUSB_DMA_LEASE lease = {0};
  init(&m);
  assert(QcomUsbInspectInherited(read32, &m, &hw) == QusbQualified);
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbStreamTranslated);
  assert(m.writes == 0);
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbQualified);
  assert(lease.active && m.writes == 1 && m.barriers == 2);
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbQualified);
  assert(s.mode == QusbDmaExactStreamBypass);
  assert(QcomUsbRestoreDma(read32, write32, barrier, &m, &lease) == QusbQualified);
  assert(!lease.active && m.writes == 2 && m.barriers == 4);
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbStreamTranslated);

  init(&m);
  set(&m, QUSB_DWC3_BASE + 0xc120, 0x33310000U);
  assert(QcomUsbInspectInherited(read32, &m, &hw) == QusbQualified);
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbQualified);
  assert(QcomUsbRestoreDma(read32, write32, barrier, &m, &lease) == QusbQualified);

  init(&m);
  set(&m, QUSB_SMMU_BASE + 0x804, 0);
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbQualified);
  assert(s.mode == QusbDmaInheritedUnmatchedBypass);
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbQualified);
  assert(m.writes == 0 && !lease.active);
  set(&m, QUSB_SMMU_BASE, 1U << 10);
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbNoExplicitStream);
  assert(m.writes == 0);
  set(&m, QUSB_SMMU_BASE + 0x804, 0x80010040U);
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbSharedStream);
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbSharedStream);
  assert(m.writes == 0);
  set(&m, QUSB_SMMU_BASE + 0xc04, 0x03010101U);
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbQualified);
  assert(s.mode == QusbDmaInheritedSharedStreamBypass && s.stream_mask == 1U);
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbQualified);
  assert(m.writes == 0 && !lease.active);
  set(&m, QUSB_SMMU_BASE + 0x804, 0x80000040U);
  set(&m, QUSB_SMMU_BASE + 0x808, 0x80000040U);
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbMultipleStreams);
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbMultipleStreams);
  assert(m.writes == 0);

  /* EXIDENABLE: SMR has full ID/MASK; validity belongs to S2CR[10]. */
  init(&m);
  set(&m, QUSB_SMMU_BASE + 0x20, (1U << 30) | (1U << 27) | (1U << 8) | 3U);
  set(&m, QUSB_SMMU_BASE, (1U << 3) | (1U << 10));
  set(&m, QUSB_SMMU_BASE + 0x804, 0x00000040U);
  set(&m, QUSB_SMMU_BASE + 0xc04, 0x03010501U);
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbQualified);
  assert(s.extended_stream_ids && s.mode == QusbDmaExactStreamBypass);
  assert(s.match_count == 1 && s.stream_id == 0x40 && s.stream_mask == 0);
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbQualified);
  assert(!m.writes && !lease.active);
  /* ID bit15 must not alias SID0x40; unlike legacy it is not reserved. */
  set(&m, QUSB_SMMU_BASE + 0x804, 0x00008040U);
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbNoExplicitStream);
  /* SMR bit31 is the mask of ID bit15, not a validity flag. */
  set(&m, QUSB_SMMU_BASE + 0x804, 0x80008040U);
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbQualified);
  assert(s.mode == QusbDmaInheritedSharedStreamBypass && s.stream_mask == 0x8000U);
  set(&m, QUSB_SMMU_BASE + 0xc04, 0x03010101U); /* EXIDVALID clear */
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbNoExplicitStream);
  set(&m, QUSB_SMMU_BASE + 0xc04, 0x03010501U);
  set(&m, QUSB_SMMU_BASE + 0x808, 0x00000040U);
  set(&m, QUSB_SMMU_BASE + 0xc08, 0x03010501U);
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbMultipleStreams);
  set(&m, QUSB_SMMU_BASE + 0xc08, 0);
  set(&m, QUSB_SMMU_BASE + 0xc04, 0x03000501U); /* translated shared */
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbSharedStream);
  set(&m, QUSB_SMMU_BASE + 0x804, 0x00000040U);
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbStreamTranslated);
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbQualified);
  assert(m.writes == 1 && lease.active && lease.extended_stream_ids);
  assert(lease.smr == 0x40U && lease.original_s2cr == 0x03000501U);
  assert(QcomUsbRestoreDma(read32, write32, barrier, &m, &lease) == QusbQualified);
  assert(m.writes == 2 && !lease.active);

  init_extended(&m);
  set(&m, QUSB_SMMU_BASE + 0x804, 0x41U);
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbNoExplicitStream);
  assert(!m.writes && !lease.active);
  init_extended(&m);
  set(&m, QUSB_SMMU_BASE + 0x804, 0x00010040U);
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbSharedStream);
  assert(!m.writes && !lease.active);
  init_extended(&m);
  set(&m, QUSB_SMMU_BASE + 0x808, 0x40U);
  set(&m, QUSB_SMMU_BASE + 0xc08, 0x03000501U);
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbMultipleStreams);
  assert(!m.writes && !lease.active);
  init_extended(&m);
  m.flip_s2cr_read=2; m.replace_s2cr=0x03000101U;
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbOwnershipChanged);
  assert(!m.writes && !lease.active);
  init_extended(&m);
  m.flip_cr0_read=2; m.replace_cr0=1U<<10;
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbOwnershipChanged);
  assert(!m.writes && !lease.active);
  init_extended(&m);
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbQualified);
  set(&m, QUSB_SMMU_BASE + 0xc04, 0x03010101U); /* validity changed */
  assert(QcomUsbRestoreDma(read32, write32, barrier, &m, &lease) == QusbOwnershipChanged);
  assert(m.writes == 1 && lease.active);
  lease.active=0;
  init_extended(&m);
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbQualified);
  set(&m, QUSB_SMMU_BASE, 1U<<10); /* EXIDENABLE changed */
  assert(QcomUsbRestoreDma(read32, write32, barrier, &m, &lease) == QusbOwnershipChanged);
  assert(m.writes == 1 && lease.active);
  set(&m, QUSB_SMMU_BASE, (1U<<10)|(1U<<3));
  assert(QcomUsbRestoreDma(read32, write32, barrier, &m, &lease) == QusbQualified);
  assert(m.writes == 2 && !lease.active);

  init(&m);
  set(&m, QUSB_SMMU_BASE + 0x3c, 0x43U); /* Implementation r4p3, not arch v4. */
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbStreamTranslated);
  set(&m, QUSB_SMMU_BASE + 0x3c, 0xffffffffU);
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbStreamTranslated);
  set(&m, QUSB_SMMU_BASE + 0x20, 0);
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbUnknownSmmu);

  init(&m);
  set(&m, QUSB_SMMU_BASE, 1U);
  assert(QcomUsbQualifyIdentityDma(read32, &m, &s) == QusbQualified);
  assert(s.mode == QusbDmaInheritedGlobalBypass && m.smr_reads == 0);
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbQualified);
  assert(m.writes == 0 && m.smr_reads == 0 && !lease.active);
  set(&m, QUSB_SMMU_BASE, 1U | (1U << 10));
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbQualified);
  assert(s.mode == QusbDmaInheritedGlobalBypass && m.writes == 0 && m.smr_reads == 0);
  set(&m, QUSB_SMMU_BASE, 8U);
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbExtendedStreamIds);
  set(&m, QUSB_SMMU_BASE, 0);
  set(&m, QUSB_SMMU_BASE + 0xc04, 2U << 16);
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbStreamFaulting);
  assert(m.writes == 0);

  init(&m);
  set(&m, QUSB_DWC3_BASE + 0xc704, 1U << 31);
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbCoreRunning);
  assert(m.writes == 0 && !lease.active);

  init(&m);
  set(&m, QUSB_DWC3_BASE + 0xc110, 1U << 12);
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbWrongCoreRole);
  assert(m.writes == 0 && !lease.active);

  init(&m);
  m.fail_write = 1;
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbWriteFailed);
  assert(m.writes == 1 && lease.active);
  assert(QcomUsbRestoreDma(read32, write32, barrier, &m, &lease) == QusbQualified);
  assert(!lease.active);

  init(&m);
  assert(QcomUsbEnterIdentityDma(read32, write32, barrier, &m, &lease, &s) == QusbQualified);
  set(&m, QUSB_SMMU_BASE + 0x804, 0x80000041U);
  assert(QcomUsbRestoreDma(read32, write32, barrier, &m, &lease) == QusbOwnershipChanged);
  assert(m.writes == 1 && lease.active);
  lease.active = 0;

  init(&m);
  set(&m, QUSB_GCC_BASE + 0x39008, 0);
  set(&m, QUSB_GCC_BASE + 0x39004, 0x80000000U);
  assert(QcomUsbInspectInherited(read32, &m, &hw) == QusbPowerDomainOff);
  assert(m.dwc_reads == 0);
  set(&m, QUSB_GCC_BASE + 0x39008, 1U << 16);
  set(&m, QUSB_GCC_BASE + 0x39018, 0x80000000U);
  assert(QcomUsbInspectInherited(read32, &m, &hw) == QusbClockOff);
  assert(m.dwc_reads == 0);
  puts("Qcom USB qualification and exclusive S2CR lease tests passed");
  return 0;
}
