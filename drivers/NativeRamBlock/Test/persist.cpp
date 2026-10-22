// Host test for the persistent overlay core: a model medium with fault injection (torn writes, lost cache, write refusal).
#include <cassert>
#include <cstdio>
#include <vector>
#include "../PersistOverlay.h"
using namespace persist;
struct Medium {
  std::vector<uint8_t> durable, cache;          // cache holds writes not yet flushed
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> pending;
  bool fua = false;                              // writes after open go straight to the medium (FUA)
  unsigned writes = 0, syncs = 0, failWriteAt = ~0u, failSyncAt = ~0u, reads = 0;
  explicit Medium(uint64_t blocks) : durable(blocks * kBlock, 0xA5) {}      // never-written medium holds junk, not zeros
  bool read(uint64_t b, uint32_t n, void *d) {
    ++reads;
    for (uint32_t i = 0; i < n; ++i) { std::vector<uint8_t> v(durable.begin() + (b + i) * kBlock, durable.begin() + (b + i + 1) * kBlock);
      for (auto &p : pending) if (p.first == b + i) v = p.second; memcpy((uint8_t *)d + i * kBlock, v.data(), kBlock); }
    return true; }
  bool write(uint64_t b, uint32_t n, const void *s) {
    if (writes++ == failWriteAt) return false;
    if (fua) { for (uint32_t i = 0; i < n; ++i) memcpy(&durable[(b + i) * kBlock], (const uint8_t *)s + i * kBlock, kBlock); return true; }
    for (uint32_t i = 0; i < n; ++i) pending.push_back({b + i, std::vector<uint8_t>((const uint8_t *)s + i * kBlock, (const uint8_t *)s + (i + 1) * kBlock)});
    return true; }
  bool sync() { if (syncs++ == failSyncAt) return false;
    for (auto &p : pending) memcpy(&durable[p.first * kBlock], p.second.data(), kBlock);
    pending.clear(); return true; }
  void powerLoss() { pending.clear(); }                                     // unflushed writes vanish
};
static std::vector<uint8_t> pageOf(uint8_t v) { return std::vector<uint8_t>(kBlock, v); }
int main() {
  const uint64_t pages = 65536 + 7;                     // forces two bitmap blocks and a ragged last byte
  uint8_t seedA[32], seedB[32]; memset(seedA, 1, 32); memset(seedB, 2, 32);
  assert(bitmapBlocks(pages) == 3);
  assert(bitmapBlocks(32768) == 1 && bitmapBlocks(32769) == 2 && bitmapBlocks(524288) == 16);
  assert(windowBlocks(524288) == 1 + 16 + 524288);
  Medium m(windowBlocks(pages));
  const size_t bytes = bitmapBlocks(pages) * kBlock;
  std::vector<uint8_t> bits(bytes);
  auto fresh = [&] { return std::vector<uint8_t>(bytes); };
  // Blank medium: read-only open must not write and reports Blank; read-write initialises it.
  { Overlay o; assert(o.open(m, pages, seedA, true, bits.data()) == Open::Blank && m.writes == 0 && !o.active && o.header == 1); }
  Overlay o; assert(o.open(m, pages, seedA, false, bits.data()) == Open::Fresh && o.active && o.present == 0 && o.generation == 1);
  m.fua = true;                                         // from here on writes are durable when they return
  // Single pages and a run; the bitmap reaches the medium inside the same call.
  assert(o.writePage(m, 0, pageOf(0x11).data()) && o.writePage(m, 40000, pageOf(0x22).data()) && o.writePage(m, pages - 1, pageOf(0x33).data()));
  assert(!o.writePage(m, pages, pageOf(0).data()));
  std::vector<uint8_t> run(8 * kBlock); for (int i = 0; i < 8; ++i) memset(&run[i * kBlock], 0x70 + i, kBlock);
  assert(o.writeRun(m, 100, 8, run.data()) && o.present == 11);
  assert(!o.writeRun(m, pages - 2, 3, run.data()));    // runs past the end are refused
  { Overlay r; auto b2 = fresh();
    assert(r.open(m, pages, seedA, true, b2.data()) == Open::Resumed && r.present == 11 && r.has(0) && r.has(40000) && r.has(pages - 1) && r.has(107) && !r.has(1) && !r.has(108));
    unsigned seen = 0; bool ok = true; std::vector<uint8_t> scratch(4 * kBlock);
    struct L { unsigned *seen; bool *ok; bool operator()(uint64_t i, const uint8_t *p) { ++*seen;
      uint8_t want = i == 0 ? 0x11 : i == 40000 ? 0x22 : i == 65542 ? 0x33 : 0x70 + (i - 100); if (p[0] != want || p[4095] != want) *ok = false; return true; } } load{&seen, &ok};
    assert(r.forEachPresent(m, load, scratch.data(), 4) && seen == 11 && ok);   // 8-page run read in two chunks of 4
    assert(!r.writePage(m, 1, pageOf(9).data()));       // read-only really is read-only
  }
  // Rewriting a present page changes no bitmap block.
  { unsigned before = m.writes; assert(o.writePage(m, 0, pageOf(0x12).data()) && m.writes == before + 1 && o.present == 11); }
  // A different seed makes the overlay stale: it is replaced, not applied.
  { Medium copy = m; Overlay r; auto b2 = fresh();
    assert(r.open(copy, pages, seedB, true, b2.data()) == Open::Blank && r.header == 3 && r.present == 0);
    assert(r.open(copy, pages, seedB, false, b2.data()) == Open::Fresh && r.generation == 2 && r.present == 0); }
  // An explicit reset discards a valid overlay for the same seed.
  { Medium copy = m; Overlay r; auto b2 = fresh();
    assert(r.open(copy, pages, seedA, false, b2.data(), true) == Open::Fresh && r.present == 0 && r.generation == 2);
    Overlay q; auto b3 = fresh(); assert(q.open(copy, pages, seedA, true, b3.data()) == Open::Resumed && q.present == 0); }
  // Page write fails: no bit is set on the medium for it.
  { Medium t = m; Overlay w; auto b2 = fresh();
    assert(w.open(t, pages, seedA, false, b2.data()) == Open::Resumed);
    t.failWriteAt = t.writes; assert(!w.writePage(t, 6, pageOf(0x55).data()));
    Overlay r; auto b3 = fresh(); assert(r.open(t, pages, seedA, true, b3.data()) == Open::Resumed && !r.has(6) && r.present == 11); }
  // Page lands, bitmap write fails: the medium has the page but not its bit, so the next boot ignores it.
  { Medium t = m; Overlay w; auto b2 = fresh();
    assert(w.open(t, pages, seedA, false, b2.data()) == Open::Resumed);
    t.failWriteAt = t.writes + 1; assert(!w.writePage(t, 7, pageOf(0x66).data()));
    Overlay r; auto b3 = fresh(); assert(r.open(t, pages, seedA, true, b3.data()) == Open::Resumed && !r.has(7) && r.present == 11); }
  // Damaged header (flip a bit): reported as damaged, read-write open reinitialises with a higher generation.
  { Medium t = m; t.durable[20] ^= 1; Overlay r; auto b2 = fresh();
    assert(r.open(t, pages, seedA, true, b2.data()) == Open::Blank && r.header == 2);
    assert(r.open(t, pages, seedA, false, b2.data()) == Open::Fresh && r.present == 0); }
  // Bits beyond the last page are corruption.
  { Medium t = m; Overlay r; auto b2 = fresh();
    t.durable[1 * kBlock + 2 * kBlock + 100] |= 0x80;    // inside bitmap block 3 (last), page index >= pages
    assert(r.open(t, pages, seedA, true, b2.data()) == Open::Failed); }
  // Failed initialisation leaves no valid header.
  { Medium t(windowBlocks(pages)); t.failSyncAt = 0; Overlay r; auto b2 = fresh();
    assert(r.open(t, pages, seedA, false, b2.data()) == Open::Failed); t.powerLoss();
    Overlay q; assert(q.open(t, pages, seedA, true, b2.data()) == Open::Blank && q.header == 1); }
  puts("PASS: overlay header, bitmap, runs, write order, stale seed, reset, failed writes");
}
