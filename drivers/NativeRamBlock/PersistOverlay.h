// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Persistent overlay over a seeded sparse disk. The seed (the packed RAM disk the loader provides) stays the immutable base; the
// pages a running system changes are stored in a window of 4096-byte blocks on some other medium and put back on the next boot.
//
// Window layout (block = 4096 bytes, all integers little endian):
//   block 0                 header: "SHNGDATA", version 1, header bytes 64, page count, SHA-256 of the seed, generation (32 bit), crc32
//   blocks 1 .. B           presence bitmap, one bit per page of the disk, B = ceil(pages / 32768)
//   blocks B+1 .. B+pages   page i at B+1+i, valid only when its bit is set
// Ops write through: write() returns only when the blocks are on the medium (the UFS store uses WRITE(10) with FUA). A run of pages
// is written first and the bitmap blocks that gained a bit after it, both before the caller's write completes, so a bit never
// promises a page that did not reach the medium, and a page the system was told is written is never lost to a crash. An
// overwrite of a page whose bit is set changes only that page.
// A header whose page count or seed digest differs from the current seed is stale and is replaced (the overlay starts empty): an
// overlay is only ever applied on top of the exact seed it was made against.
//
// Ops: bool read(uint64_t block, uint32_t count, void *dst); bool write(uint64_t block, uint32_t count, const void *src);
//      bool sync();   (blocks are relative to the window; sync is only used while the header is made)
#include <stdint.h>
#include <string.h>

namespace persist {
constexpr uint32_t kBlock = 4096;
constexpr uint32_t kHeaderBytes = 64;

inline uint32_t crc32 (const uint8_t *data, size_t bytes)
{
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < bytes; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
  }
  return ~crc;
}
inline void put32 (uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t (v >> (8 * i)); }
inline void put64 (uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t (v >> (8 * i)); }
inline uint32_t get32 (const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; --i) v = (v << 8) | p[i]; return v; }
inline uint64_t get64 (const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | p[i]; return v; }

inline uint64_t bitmapBlocks (uint64_t pages) { return (pages + 8ULL * kBlock - 1) / (8ULL * kBlock); }
inline uint64_t windowBlocks (uint64_t pages) { return 1 + bitmapBlocks (pages) + pages; }
inline uint64_t pageBlock (uint64_t pages, uint64_t index) { return 1 + bitmapBlocks (pages) + index; }

inline void encodeHeader (uint8_t block[kBlock], uint64_t pages, const uint8_t seed[32], uint64_t generation)
{
  memset (block, 0, kBlock);
  memcpy (block, "SHNGDATA", 8);
  put32 (block + 8, 1);
  put32 (block + 12, kHeaderBytes);
  put64 (block + 16, pages);
  memcpy (block + 24, seed, 32);
  put32 (block + 56, uint32_t (generation));
  // The CRC covers the first 60 bytes (the generation included); bytes 60..63 hold it.
  put32 (block + 60, crc32 (block, 60));
}
// 0 valid and for this seed; 1 blank or foreign (no magic); 2 damaged (CRC, version, size); 3 valid but for another seed/size.
inline int checkHeader (const uint8_t block[kBlock], uint64_t pages, const uint8_t seed[32], uint64_t *generation)
{
  if (memcmp (block, "SHNGDATA", 8)) return 1;
  if (get32 (block + 8) != 1 || get32 (block + 12) != kHeaderBytes || get32 (block + 60) != crc32 (block, 60)) return 2;
  if (generation) *generation = get32 (block + 56);
  if (get64 (block + 16) != pages || memcmp (block + 24, seed, 32)) return 3;
  return 0;
}

enum class Open { Resumed, Fresh, Blank, Failed };

struct Overlay {
  uint64_t pages = 0, generation = 0, present = 0;
  uint8_t *bitmap = nullptr;       // bitmapBlocks(pages) * kBlock bytes, owned by the caller
  // Write-back (fajita, native-data-writeback=1): with `dirty` set (bitmapBlocks(pages) bytes, owned by the caller), writeRun only
  // marks the bitmap blocks that gained a bit and flush() commits them: cache flush (the pages are durable), the bitmap blocks, cache
  // flush. A bit never reaches the medium before its page, so a reset loses only writes no flush has acknowledged.
  uint8_t *dirty = nullptr;
  uint64_t dirtyCount = 0;
  bool readOnly = true, active = false;
  int header = -1;                 // checkHeader result at open

  bool has (uint64_t i) const { return bitmap[i >> 3] >> (i & 7) & 1; }

  // `readOnly`: never write; a stale or blank window is reported (Blank) and left alone. Otherwise it is initialised: bitmap
  // first, header last, so a tear leaves no valid header.
  template <class Ops> Open open (Ops &ops, uint64_t pageCount, const uint8_t seed[32], bool ro, uint8_t *bits, bool discard = false)
  {
    pages = pageCount; bitmap = bits; readOnly = ro; present = 0; active = false;
    uint64_t bb = bitmapBlocks (pages);
    uint8_t block[kBlock];
    if (!pages || !ops.read (0, 1, block)) return Open::Failed;
    header = checkHeader (block, pages, seed, &generation);
    if (header == 0 && discard && !ro) header = 3;   // the caller asked to start over: handled like a stale overlay
    if (header == 0) {
      for (uint64_t b = 0; b < bb; ++b) if (!ops.read (1 + b, 1, bitmap + b * kBlock)) return Open::Failed;
      // Bits past the last page must be clear, or the loader would read blocks outside the window.
      for (uint64_t i = pages; i < bb * 8ULL * kBlock; ++i) if (has (i)) return Open::Failed;
      for (uint64_t i = 0; i < pages; ++i) present += has (i);
      active = true;
      return Open::Resumed;
    }
    memset (bitmap, 0, bb * kBlock);
    if (ro) return Open::Blank;
    uint8_t zero[kBlock] = {};
    for (uint64_t b = 0; b < bb; ++b) if (!ops.write (1 + b, 1, zero)) return Open::Failed;
    if (!ops.sync ()) return Open::Failed;
    generation = (header == 3 || header == 2) ? generation + 1 : 1;
    encodeHeader (block, pages, seed, generation);
    if (!ops.write (0, 1, block) || !ops.sync ()) return Open::Failed;
    active = true;
    return Open::Fresh;
  }

  // Calls load(index, page) for every present page, in order, reading runs of up to maxRun consecutive present pages with one
  // read into scratch (maxRun * kBlock bytes). Stops at the first failure.
  template <class Ops, class Load> bool forEachPresent (Ops &ops, Load &load, uint8_t *scratch, uint32_t maxRun)
  {
    if (!maxRun) return false;
    for (uint64_t i = 0; i < pages;) {
      if (!has (i)) { ++i; continue; }
      uint32_t n = 1;
      while (n < maxRun && i + n < pages && has (i + n)) ++n;
      if (!ops.read (pageBlock (pages, i), n, scratch)) return false;
      for (uint32_t k = 0; k < n; ++k) if (!load (i + k, scratch + (uint64_t) k * kBlock)) return false;
      i += n;
    }
    return true;
  }

  // Writes `count` consecutive pages starting at `index` (count * kBlock bytes at data), then the bitmap blocks that gained a bit.
  // On false the run may be partly on the medium but no bit names a page that is not; the caller must stop using the overlay.
  template <class Ops> bool writeRun (Ops &ops, uint64_t index, uint32_t count, const void *data)
  {
    if (readOnly || !active || !count || index >= pages || count > pages - index) return false;
    if (!ops.write (pageBlock (pages, index), count, data)) return false;
    uint64_t firstBlock = ~0ULL, lastBlock = 0;
    for (uint64_t i = index; i < index + count; ++i) {
      if (has (i)) continue;
      uint64_t b = i / (8ULL * kBlock);
      if (firstBlock == ~0ULL) firstBlock = b;
      lastBlock = b;
    }
    // Set the bits only after the pages are durable; the bitmap blocks follow before this call returns.
    for (uint64_t i = index; i < index + count; ++i)
      if (!has (i)) { bitmap[i >> 3] |= uint8_t (1 << (i & 7)); ++present; }
    if (firstBlock == ~0ULL) return true;
    if (dirty) {
      for (uint64_t b = firstBlock; b <= lastBlock; ++b) if (!dirty[b]) { dirty[b] = 1; ++dirtyCount; }
      return true;
    }
    for (uint64_t b = firstBlock; b <= lastBlock; ++b) if (!ops.write (1 + b, 1, bitmap + b * kBlock)) return false;
    return true;
  }

  // Write-back commit (see `dirty`): everything written before this call is on the medium, with its bits, when it returns true.
  template <class Ops> bool flush (Ops &ops)
  {
    if (readOnly || !active) return true;
    if (!ops.sync ()) return false;
    if (!dirty || !dirtyCount) return true;
    const uint64_t bb = bitmapBlocks (pages);
    for (uint64_t b = 0; b < bb; ++b) {
      if (!dirty[b]) continue;
      if (!ops.write (1 + b, 1, bitmap + b * kBlock)) return false;
      dirty[b] = 0; --dirtyCount;
    }
    return ops.sync ();
  }
  template <class Ops> bool writePage (Ops &ops, uint64_t index, const void *page) { return writeRun (ops, index, 1, page); }
};
}  // namespace persist
