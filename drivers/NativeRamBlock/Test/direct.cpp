// Host test of DirectDisk.h: random reads and writes against a flat byte-array oracle, with the seed kept immutable and a store
// that can fail.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../DirectDisk.h"
using namespace persist;
struct Medium {
  std::vector<uint8_t> blocks;
  unsigned writes = 0, reads = 0, failWriteAt = ~0u, failReadAt = ~0u;
  explicit Medium(uint64_t n) : blocks(n * kBlock, 0xA5) {}
  bool read(uint64_t b, uint32_t n, void *d) { if (reads++ == failReadAt) return false; memcpy(d, &blocks[b * kBlock], (size_t) n * kBlock); return true; }
  bool write(uint64_t b, uint32_t n, const void *s) { if (writes++ == failWriteAt) return false; memcpy(&blocks[b * kBlock], s, (size_t) n * kBlock); return true; }
  bool sync() { return true; }
};
struct Buffer {
  std::vector<uint8_t> &data; uint64_t limit = ~0ULL;     // limit: bytes the buffer will actually move (short-copy injection)
  uint64_t read(uint64_t at, void *p, uint64_t n) { if (at + n > limit) n = at > limit ? 0 : limit - at; memcpy(p, &data[at], n); return n; }
  uint64_t copy(uint64_t at, const void *p, uint64_t n) { if (at + n > limit) n = at > limit ? 0 : limit - at; memcpy(&data[at], p, n); return n; }
};
int main() {
  const uint64_t pages = 300;                            // 1.2 MiB logical disk
  RamSparseDisk seed = {}; seed.bytes = pages * 4096; std::vector<uint8_t *> seedPages(pages, nullptr); seed.pages = seedPages.data();
  std::vector<uint8_t> oracle(seed.bytes, 0);
  srand(7);
  for (uint64_t i = 0; i < pages; i += 3) {              // every third page has seed content
    seedPages[i] = (uint8_t *) malloc(4096);
    for (int k = 0; k < 4096; ++k) seedPages[i][k] = (uint8_t) (rand() | 1);
    memcpy(&oracle[i * 4096], seedPages[i], 4096);
  }
  std::vector<uint8_t> seedCopy(seed.bytes, 0);
  for (uint64_t i = 0; i < pages; ++i) if (seedPages[i]) memcpy(&seedCopy[i * 4096], seedPages[i], 4096);
  uint8_t digest[32] = {9};
  Medium m(windowBlocks(pages));
  std::vector<uint8_t> bits(bitmapBlocks(pages) * kBlock), stage(8 * kBlock);
  Overlay ov; assert(ov.open(m, pages, digest, false, bits.data()) == Open::Fresh);
  bool storeError = false;
  std::vector<uint8_t> io(300 * 1024);
  for (int op = 0; op < 4000; ++op) {
    uint64_t length = 1 + (uint64_t) rand() % (op % 7 == 0 ? 120000 : 9000), offset = (uint64_t) rand() % (seed.bytes - length);
    if (op % 5 == 0) offset &= ~4095ULL;                  // page aligned
    if (op % 11 == 0) length = (length + 4095) & ~4095ULL, offset &= ~4095ULL, length = length > seed.bytes - offset ? seed.bytes - offset : length;
    Buffer b{io};
    if (rand() & 1) {
      for (uint64_t k = 0; k < length; ++k) io[k] = (uint8_t) rand();
      assert(directDiskIO(seed, ov, m, stage.data(), 8, true, offset, length, b, &storeError) == length && !storeError);
      memcpy(&oracle[offset], io.data(), length);
    } else {
      memset(io.data(), 0xEE, length);
      assert(directDiskIO(seed, ov, m, stage.data(), 8, false, offset, length, b, &storeError) == length && !storeError);
      assert(!memcmp(io.data(), &oracle[offset], length));
    }
    for (uint64_t i = 0; i < pages; i += 17) if (seedPages[i]) assert(!memcmp(seedPages[i], &seedCopy[i * 4096], 4096));   // seed untouched
  }
  // Whole disk equals the oracle.
  for (uint64_t at = 0; at < seed.bytes; at += 100000) {
    uint64_t n = seed.bytes - at < 100000 ? seed.bytes - at : 100000; Buffer b{io};
    assert(directDiskIO(seed, ov, m, stage.data(), 8, false, at, n, b, &storeError) == n && !memcmp(io.data(), &oracle[at], n));
  }
  // The bitmap on the medium alone reproduces the disk: reopen read-only on a fresh seed copy.
  { Overlay r; std::vector<uint8_t> b2(bits.size()); assert(r.open(m, pages, digest, true, b2.data()) == Open::Resumed && r.present == ov.present);
    for (uint64_t at = 0; at < seed.bytes; at += 70000) { uint64_t n = seed.bytes - at < 70000 ? seed.bytes - at : 70000; Buffer b{io};
      bool e; r.readOnly = false; assert(directDiskIO(seed, r, m, stage.data(), 8, false, at, n, b, &e) == n && !memcmp(io.data(), &oracle[at], n)); } }
  // Refusals: a failing store write is reported and nothing is counted; a short copy is not a store error; read-only refuses writes.
  { Buffer b{io}; for (uint64_t k = 0; k < 8192; ++k) io[k] = 0x77;
    m.failWriteAt = m.writes; assert(directDiskIO(seed, ov, m, stage.data(), 8, true, 4096, 8192, b, &storeError) == 0 && storeError);
    m.failWriteAt = ~0u; storeError = false;
    b.limit = 100; assert(directDiskIO(seed, ov, m, stage.data(), 8, true, 0, 8192, b, &storeError) == 0 && !storeError);
    b.limit = ~0ULL; m.failReadAt = m.reads;
    uint64_t firstPresent = 0; while (!ov.has(firstPresent)) ++firstPresent;
    assert(directDiskIO(seed, ov, m, stage.data(), 8, false, firstPresent * 4096, 4096, b, &storeError) == 0 && storeError);
    m.failReadAt = ~0u; ov.readOnly = true; storeError = false;
    assert(directDiskIO(seed, ov, m, stage.data(), 8, true, 0, 4096, b, &storeError) == 0); ov.readOnly = false; }
  assert(m.writes > 0 && m.reads > 0);
  puts("PASS: direct Data disk against a flat oracle: runs, partial pages, seed immutability, reopen, store failures, short copies");
  for (auto p : seedPages) free(p);
  return 0;
}
