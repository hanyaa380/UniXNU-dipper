/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "soc/QcomUsbDmaDomain.h"

enum {
  SM845_STREAM_COUNT = 128,
  SM845_PAGE_COUNT = 64,
  SM845_BANK_COUNT = 64
};

struct model {
  uint8_t smmu[QUSB_SMMU_BYTES];
  QUSB_DOMAIN_LEASE *lease;
  unsigned int cleaned;
};

static QUSB_U32 load32(const void *p)
{
  QUSB_U32 value;
  memcpy(&value, p, sizeof(value));
  return value;
}

static QUSB_U64 load64(const void *p)
{
  QUSB_U64 value;
  memcpy(&value, p, sizeof(value));
  return value;
}

static void store32(void *p, QUSB_U32 value)
{
  memcpy(p, &value, sizeof(value));
}

static void store64(void *p, QUSB_U64 value)
{
  memcpy(p, &value, sizeof(value));
}

static int mmio_read32(void *cookie, QUSB_U64 address, QUSB_U32 *value)
{
  struct model *m = cookie;
  if (address >= QUSB_GCC_BASE && address < QUSB_GCC_BASE + QUD_GCC_BYTES) {
    QUSB_U64 offset = address - QUSB_GCC_BASE;
    *value = offset == QUD_GCC_CFG_GDSCR_OFFSET ? (1U << 16) : 0;
    return 1;
  }
  if (address >= QUSB_DWC3_BASE && address < QUSB_DWC3_BASE + QUD_DWC3_BYTES) {
    QUSB_U64 offset = address - QUSB_DWC3_BASE;
    if (offset == 0xc120) *value = 0x5533330aU;
    else if (offset == 0xc110) *value = 2U << 12;
    else if (offset == 0xc70c) *value = 1U << 22;
    else *value = 0;
    return 1;
  }
  if (address < QUSB_SMMU_BASE || address - QUSB_SMMU_BASE > QUSB_SMMU_BYTES - 4)
    return 0;
  {
    QUSB_U64 offset = address - QUSB_SMMU_BASE;
    QUSB_U64 body = offset & ~4095ULL;
    QUSB_U64 page = body >> 12;
    if ((offset & 4095U) == 0x58U && page >= SM845_PAGE_COUNT &&
        page < SM845_PAGE_COUNT + SM845_BANK_COUNT &&
        (load32(m->smmu + body) & 1U)) {
      *value = 0x400U;
    } else {
      *value = load32(m->smmu + offset);
    }
    return 1;
  }
}

static int mmio_write32(void *cookie, QUSB_U64 address, QUSB_U32 value)
{
  struct model *m = cookie;
  if (address < QUSB_SMMU_BASE || address - QUSB_SMMU_BASE > QUSB_SMMU_BYTES - 4)
    return 0;
  assert(QcomUsbDomainWriteAllowed(m->lease, address, value, 4));
  store32(m->smmu + address - QUSB_SMMU_BASE, value);
  return 1;
}

static int mmio_read64(void *cookie, QUSB_U64 address, QUSB_U64 *value)
{
  struct model *m = cookie;
  if (address < QUSB_SMMU_BASE || address - QUSB_SMMU_BASE > QUSB_SMMU_BYTES - 8)
    return 0;
  *value = load64(m->smmu + address - QUSB_SMMU_BASE);
  return 1;
}

static int mmio_write64(void *cookie, QUSB_U64 address, QUSB_U64 value)
{
  struct model *m = cookie;
  if (address < QUSB_SMMU_BASE || address - QUSB_SMMU_BASE > QUSB_SMMU_BYTES - 8)
    return 0;
  assert(QcomUsbDomainWriteAllowed(m->lease, address, value, 8));
  store64(m->smmu + address - QUSB_SMMU_BASE, value);
  return 1;
}

static void barrier(void *cookie)
{
  (void)cookie;
}

static void delay(void *cookie, QUSB_U32 microseconds)
{
  (void)cookie;
  (void)microseconds;
}

static int clean(void *cookie, void *cpu, QUSB_U32 bytes)
{
  struct model *m = cookie;
  assert(cpu && bytes == QUSB_DOMAIN_PT_BYTES);
  m->cleaned = 1;
  return 1;
}

int main(void)
{
  struct model model;
  QUSB_DOMAIN_LEASE lease;
  QUSB_INHERITED inherited;
  QUSB_DOMAIN_PLATFORM platform;
  void *tables = NULL;
  const QUSB_U64 table_pa = 0x123450000ULL;
  const QUSB_U64 dma_pa = 0x234560000ULL;

  memset(&model, 0, sizeof(model));
  memset(&lease, 0, sizeof(lease));
  model.lease = &lease;
  assert(!posix_memalign(&tables, 4096, QUSB_DOMAIN_PT_BYTES));

  store32(model.smmu + 0x20, (1U << 30) | (1U << 27) | SM845_STREAM_COUNT);
  store32(model.smmu + 0x24, (5U << 28) | SM845_BANK_COUNT);
  store32(model.smmu + 0x28, 0x5111U);
  store32(model.smmu, 0x400U);

  assert(QcomUsbInspectInherited(mmio_read32, &model, &inherited) == QusbQualified);

  platform = (QUSB_DOMAIN_PLATFORM){
    &model, mmio_read32, mmio_write32, mmio_read64, mmio_write64,
    barrier, delay, clean
  };
  assert(QcomUsbDomainInstall(&platform, tables, table_pa,
      QUSB_DOMAIN_PT_BYTES, dma_pa, QUSB_DOMAIN_DMA_BYTES, &lease) == QusbDomainOkay);
  assert(model.cleaned && lease.active);
  assert(lease.stream_count == SM845_STREAM_COUNT);
  assert(lease.bank_count == SM845_BANK_COUNT);
  assert(lease.index == 0 && lease.cb == 0);
  assert(load32(model.smmu + 0x800) == (0x80000000U | QUD_STREAM_ID));
  assert(load32(model.smmu + 0xc00) == 0);
  assert(load32(model.smmu + (SM845_PAGE_COUNT * 4096U)) == 0x27U);
  assert(QcomUsbDomainRestore(&platform, &lease) == QusbDomainOkay);
  assert(!lease.active);
  assert(load32(model.smmu + 0x800) == 0);
  assert(load32(model.smmu + 0xc00) == 0);
  assert(load32(model.smmu + (SM845_PAGE_COUNT * 4096U)) == 0);

  free(tables);
  return 0;
}
