// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// The seed of a large persistent Data disk: only its nonzero pages are kept, sorted by page index, so a disk of hundreds of GiB costs memory
// in proportion to its content (a packed seed holds a few thousand pages), not to its size. Used with DirectDisk.h; the array-backed
// RamSparseDisk (SparseDisk.h) stays for volatile disks of at most a few GiB.
#include <stdint.h>
#include <string.h>
#include "SparseDisk.h"

struct SeedDisk {
  uint64_t bytes = 0, count = 0, capacity = 0, resident = 0;
  uint64_t *index = nullptr;       // capacity entries, ascending, count used
  uint8_t **page = nullptr;
  const uint8_t *find(uint64_t i) const {
    uint64_t lo = 0, hi = count;
    while (lo < hi) {
      uint64_t mid = lo + (hi - lo) / 2;
      if (index[mid] < i) lo = mid + 1; else hi = mid;
    }
    return lo < count && index[lo] == i ? page[lo] : nullptr;
  }
};

// Nonzero pages of a validated packed seed (ramPackedLayout): the arrays' capacity.
static bool seedCountNonzero(const uint8_t *p, uint64_t n, uint64_t *pages) {
  uint64_t size = 0, total = 0;
  if (!ramPackedLayout(p, n, &size)) return false;
  uint64_t count = ramU64(p + 24), cursor = 32 + count * 16;
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t len = ramU64(p + 40 + i * 16);
    for (uint64_t j = 0; j < len; j += 4096) {
      const uint8_t *src = p + cursor + j;
      uint8_t any = 0;
      for (unsigned k = 0; k < 4096; ++k) any |= src[k];
      if (any) ++total;
    }
    cursor += len;
  }
  *pages = total;
  return true;
}

// Ops.allocate() returns a 4096-byte block or null. The arrays are the caller's (capacity from seedCountNonzero).
template<class Ops> static bool seedImportSparse(SeedDisk &d, const uint8_t *p, uint64_t n, Ops &ops) {
  uint64_t size = 0;
  if (!d.index || !d.page || d.count || !ramPackedLayout(p, n, &size) || d.bytes != size) return false;
  uint64_t count = ramU64(p + 24), cursor = 32 + count * 16;
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t at = ramU64(p + 32 + i * 16), len = ramU64(p + 40 + i * 16);
    for (uint64_t j = 0; j < len; j += 4096) {
      const uint8_t *src = p + cursor + j;
      uint8_t any = 0;
      for (unsigned k = 0; k < 4096; ++k) any |= src[k];
      if (!any) continue;
      if (d.count >= d.capacity) return false;
      uint8_t *copy = ops.allocate();
      if (!copy) return false;
      memcpy(copy, src, 4096);
      d.index[d.count] = (at + j) / 4096;
      d.page[d.count] = copy;
      ++d.count;
      d.resident += 4096;
    }
    cursor += len;
  }
  return true;
}

// The identity of the seed for the persistent store: a disk of at most 2 GiB keeps the plain hash of all its bytes (as every earlier image and
// the offline checks use); a larger one hashes its size and its nonzero pages with their indexes, so that hashing a 200 GiB disk of zeros
// does not take minutes. Sha is a SHA-256 context type with init/update/final through the callbacks.
template<class Ctx, class Init, class Update, class Final> static void seedIdentity(const SeedDisk &d, Ctx &ctx, Init init, Update update, Final fin, uint8_t out[32]) {
  static const uint8_t zero[4096] = {};
  init(ctx);
  if (d.bytes <= 0x80000000ULL) {
    uint64_t next = 0;
    for (uint64_t k = 0; k < d.count; ++k) {
      for (; next < d.index[k]; ++next) update(ctx, zero, 4096);
      update(ctx, d.page[k], 4096);
      ++next;
    }
    for (; next < d.bytes / 4096; ++next) update(ctx, zero, 4096);
  } else {
    uint8_t head[17] = {'S','H','N','G','S','E','E','D','2'};
    for (unsigned i = 0; i < 8; ++i) head[9 + i] = (uint8_t) (d.bytes >> (8 * i));
    update(ctx, head, 17);
    for (uint64_t k = 0; k < d.count; ++k) {
      uint8_t idx[8];
      for (unsigned i = 0; i < 8; ++i) idx[i] = (uint8_t) (d.index[k] >> (8 * i));
      update(ctx, idx, 8);
      update(ctx, d.page[k], 4096);
    }
  }
  fin(ctx, out);
}
