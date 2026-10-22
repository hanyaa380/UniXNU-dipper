// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// I/O for a Data disk whose changed pages live only in the store window (PersistOverlay.h), not in RAM. The seed (the sparse disk) stays the
// immutable base for pages the window does not hold: a page whose bit is set is read from the window, any other page from the seed (or zero).
// Writes always go to the window, durably, before they are reported done; the seed is never modified.
//   Buf: uint64_t read(uint64_t at, void *p, uint64_t n)        bytes of the request buffer (the data of a write)
//        uint64_t copy(uint64_t at, const void *p, uint64_t n)  bytes into the request buffer (the data of a read)
//   Store: bool read(block, count, dst); bool write(block, count, src)   (PersistOverlay.h's Ops)
// `stage` is stagePages * 4096 bytes. Returns the bytes completed; *storeError is set when the store refused or failed (a short
// copy by the buffer is not a store error). Caller serializes I/O.
#include "SparseDisk.h"
#include "SeedDisk.h"
#include "PersistOverlay.h"

template<class Seed, class Store, class Buf>
static uint64_t directDiskIO(const Seed &seed, persist::Overlay &ov, Store &store, uint8_t *stage, uint32_t stagePages, bool write,
                             uint64_t offset, uint64_t length, Buf &buf, bool *storeError)
{
  static const uint8_t zero[4096] = {};
  *storeError = false;
  if (!stagePages || !length || offset > seed.bytes || length > seed.bytes - offset) return 0;
  if (!ov.active || (write && ov.readOnly)) return 0;
  const uint64_t lastIndex = (offset + length - 1) / 4096;
  uint64_t done = 0;
  while (done < length) {
    uint64_t at = offset + done, index = at / 4096, skip = at & 4095;
    if (!write) {
      if (ov.has(index)) {
        uint32_t n = 1;
        while (n < stagePages && index + n <= lastIndex && ov.has(index + n)) ++n;
        if (!store.read(persist::pageBlock(ov.pages, index), n, stage)) { *storeError = true; break; }
        uint64_t avail = (uint64_t) n * 4096 - skip;
        if (avail > length - done) avail = length - done;
        uint64_t got = buf.copy(done, stage + skip, avail);
        done += got;
        if (got != avail) break;
        continue;
      }
      uint64_t n = 4096 - skip;
      if (n > length - done) n = length - done;
      const uint8_t *page = seed.find(index);
      uint64_t got = buf.copy(done, (page ? page : zero) + skip, n);
      done += got;
      if (got != n) break;
      continue;
    }
    if (skip == 0 && length - done >= 4096) {          // whole pages: one run, one durable commit
      uint32_t n = 0;
      while (n < stagePages && (uint64_t) (n + 1) * 4096 <= length - done) ++n;
      if (buf.read(done, stage, (uint64_t) n * 4096) != (uint64_t) n * 4096) break;
      if (!ov.writeRun(store, index, n, stage)) { *storeError = true; break; }
      done += (uint64_t) n * 4096;
      continue;
    }
    uint64_t n = 4096 - skip;                           // part of a page: read, merge, write back
    if (n > length - done) n = length - done;
    if (ov.has(index)) {
      if (!store.read(persist::pageBlock(ov.pages, index), 1, stage)) { *storeError = true; break; }
    } else {
      const uint8_t *page = seed.find(index);
      memcpy(stage, page ? page : zero, 4096);
    }
    if (buf.read(done, stage + skip, n) != n) break;
    if (!ov.writeRun(store, index, 1, stage)) { *storeError = true; break; }
    done += n;
  }
  return done;
}
