// SPDX-License-Identifier: BSD-2-Clause-Patent
// Plan before any SMMU write. Interface register facts: ARM SMMUv2.
struct UfsSmmuPlan {
  unsigned slot = 0xffffffffU, bank = 0, pages = 0;
  uint32_t smr = 0, s2cr = 0;
  bool reuse = false;
  bool firmware = false;   // the firmware's own entry for the stream, kept as it is (no SMMU write)
  const char *error = nullptr;
};
// ARM IHI 0062D.c sections 1.6 and 3.7: VA64=0 still rejects inputs above
// 32 bits with SCTLR.M=0. With VA64=1 the bypass range is bounded by all
// three hardware widths, not TCR2.PASize (which only limits translation).
static unsigned ufsSmmuAddressBits(unsigned code)
{
  switch (code) {
    case 0: return 32; case 1: return 36; case 2: return 40;
    case 3: return 42; case 4: return 44; case 5: return 48;
    default: return 0;
  }
}
// The stream-match register for the UFS stream: valid, the ID bits the SMMU ignores (mask) and the ID. A board
// whose UFS host emits several IDs (SDM845: 0x100 with mask 0xf) needs the mask, or the other IDs stay unmatched.
static uint32_t ufsSmmuSmr(uint16_t stream, uint16_t mask)
{
  return 0x80000000U | (uint32_t(mask & 0x7fff) << 16) | (stream & ~mask & 0xffff);
}
// One attempt with a given context bank (counted from the top). The caller falls back to lower banks when this one is held by another stream.
// window is the size of the SMMU register mapping, which the context banks must fit in.
template <class Read>
static UfsSmmuPlan planUfsSmmuBank(uint16_t stream, uint16_t mask, uint64_t window, uint64_t dma, uint64_t bytes, Read read,
                                   unsigned bankFromTop, bool adoptFirmware = false)
{
  UfsSmmuPlan p;
  uint32_t idr0 = read(0x20), idr2 = read(0x28);
  unsigned count = idr0 & 255;
  if (!(idr0 & (1U << 30)) || !(idr2 & (1U << 12))) {
    p.error = "SMMU AArch64 stage 1 unsupported"; return p;
  }
  for (unsigned shift = 0; shift <= 8; shift += 4) {
    unsigned bits = ufsSmmuAddressBits((idr2 >> shift) & 15);
    if (!bits || !bytes || dma >= (1ULL << bits) || bytes > (1ULL << bits) - dma) {
      p.error = "DMA exceeds SMMU address width"; return p;
    }
  }
  uint32_t idr1 = read(0x24);
  unsigned banks = idr1 & 255;
  p.pages = 1U << (((idr1 >> 28) & 7) + 1);
  if ((idr1 >> 31) || !count || p.pages > 128 || banks < 2 || banks > 128 ||
      (p.pages + banks) * 4096ULL > window) {
    p.error = "unsupported SMMU geometry"; return p;
  }
  if (count > 128) count = 128; // Firmware's reliably accessible groups only.
  if (bankFromTop >= banks - 1) {p.error = "no free context bank"; return p;}
  p.bank = banks - 1 - bankFromTop;
  unsigned matches = 0;
  for (unsigned i = 0; i < count; ++i) {
    uint32_t smr = read(0x800 + 4 * i);
    if (!(smr & 0x80000000U)) {
      if (!p.reuse) p.slot = i;
      continue;
    }
    uint32_t s2cr = read(0xc00 + 4 * i);
    uint32_t smrMask = (smr >> 16) & 0x7fff;
    // Any ID both entries match is an overlap; only our exact entry on our bank is ours to reuse.
    bool match = ((smr ^ stream) & 0xffff & ~(smrMask | mask)) == 0;
    bool ourBank = ((s2cr >> 16) & 3) == 0 && (s2cr & 255) == p.bank;
    if (match) {
      ++matches;
      // SDM845: the firmware's UFS driver leaves exactly this stream's entry on a context bank of its own, translation off
      // (its DMA goes through it). That bank already passes physical addresses; adopt it without writing anything.
      unsigned fwBank = s2cr & 255;
      if (adoptFirmware && bankFromTop == 0 && matches == 1 && smr == ufsSmmuSmr(stream, mask) && !ourBank &&
          ((s2cr >> 16) & 3) == 0 && fwBank < banks && fwBank != p.bank &&
          !(read((p.pages + fwBank) * 4096) & 1)) {
        bool va64 = read(0x1800 + 4 * fwBank) & 1;
        if (!va64 && (dma >= (1ULL << 32) || bytes > (1ULL << 32) - dma)) {
          p.slot = i; p.smr = smr; p.s2cr = s2cr;
          p.error = "firmware UFS bypass bank is 32-bit and DMA is above 4 GiB"; return p;
        }
        p.reuse = true; p.firmware = true; p.slot = i; p.bank = fwBank; p.smr = smr; p.s2cr = s2cr;
        continue;
      }
      if (matches > 1 || smr != ufsSmmuSmr(stream, mask) || !ourBank) {
        p.slot = i; p.smr = smr; p.s2cr = s2cr;
        p.error = "conflicting UFS stream mapping"; return p;
      }
      p.reuse = true; p.slot = i;
    } else if (ourBank) {
      p.slot = i; p.smr = smr; p.s2cr = s2cr;
      p.error = "reserved context bank already used"; return p;
    }
  }
  if (p.firmware) return p;
  uint32_t sctlr = read((p.pages + p.bank) * 4096);
  if ((sctlr & 1) || (p.reuse && (sctlr || read(0x1000 + 4 * p.bank) != 0x10000 ||
                                (read(0x1800 + 4 * p.bank) & ~1U)))) {
    p.error = "reserved context bank not bypass"; return p;
  }
  if (p.slot == 0xffffffffU) p.error = "no free stream-match group";
  return p;
}

// The UFS stream's context bank: the top bank first. A stream mapping left behind by an earlier run (or by another OS that used the SMMU before a
// warm reset) can still point another stream at that bank; rather than refuse to boot, the next banks down are tried, each held to the same checks
// (no other valid stream maps it, translation off). Any other refusal (a conflicting mapping of our own stream, no free group, unsupported geometry)
// is final and is reported as the first attempt saw it.
template <class Read>
static UfsSmmuPlan planUfsSmmu(uint16_t stream, uint16_t mask, uint64_t window, uint64_t dma, uint64_t bytes, Read read,
                               bool adoptFirmware = false)
{
  enum {kBankAttempts = 8};
  UfsSmmuPlan first = planUfsSmmuBank(stream, mask, window, dma, bytes, read, 0, adoptFirmware);
  if (!first.error) return first;
  // "conflicting" is retried too: our own stream may already sit on a lower bank from an earlier run that had to fall back.
  auto retryable = [](const char *e) {
    return !__builtin_strcmp(e, "reserved context bank already used") || !__builtin_strcmp(e, "reserved context bank not bypass") ||
           !__builtin_strcmp(e, "conflicting UFS stream mapping");
  };
  if (!retryable(first.error)) return first;
  for (unsigned attempt = 1; attempt < kBankAttempts; ++attempt) {
    UfsSmmuPlan next = planUfsSmmuBank(stream, mask, window, dma, bytes, read, attempt);
    if (!next.error) return next;
    if (!retryable(next.error)) break;
  }
  return first;
}

// The caller must first quiesce the UFS queue. The planner refuses other
// streams sharing this bank and enabled translation; no live context is
// disabled here. Write callbacks must complete an MMIO barrier before return.
template <class Read, class Write>
static const char *configureUfsSmmu(const UfsSmmuPlan &p, uint16_t stream, uint16_t mask, Read read, Write write)
{
  if (p.error) return p.error;
  unsigned cb = (p.pages + p.bank) * 4096;
  if (p.firmware) {
    // Nothing is written: the firmware's entry and bank must still be what the plan saw.
    if (read(0x800 + 4 * p.slot) != ufsSmmuSmr(stream, mask) || read(0xc00 + 4 * p.slot) != p.s2cr || (read(cb) & 1))
      return "firmware UFS stream mapping changed";
    return nullptr;
  }
  if (!p.reuse) write(cb, 0);
  if (read(cb) != 0) return "SMMU bypass disable readback";
  // Set VA64 before exposing a newly allocated stream. Repair an exclusive
  // existing 32-bit bypass too; do not silently reuse the faulty state.
  if (read(0x1800 + 4 * p.bank) != 1) write(0x1800 + 4 * p.bank, 1);
  if (!p.reuse) write(0x1000 + 4 * p.bank, 0x10000);
  if (read(0x1800 + 4 * p.bank) != 1 || read(0x1000 + 4 * p.bank) != 0x10000 || read(cb) != 0)
    return "SMMU VA64 bypass readback";
  if (!p.reuse) {
    write(0xc00 + 4 * p.slot, p.bank);
    if (read(0xc00 + 4 * p.slot) != p.bank) return "SMMU stream context readback";
    write(0x800 + 4 * p.slot, ufsSmmuSmr(stream, mask));
  }
  if (read(0x800 + 4 * p.slot) != ufsSmmuSmr(stream, mask) ||
      read(0xc00 + 4 * p.slot) != p.bank)
    return "SMMU stream mapping readback";
  return nullptr;
}
