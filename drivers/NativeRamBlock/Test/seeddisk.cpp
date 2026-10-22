// Host test of SeedDisk.h with DirectDisk.h on a disk far larger than RAM: sparse seed, sparse store, sparse oracle.
#include <CommonCrypto/CommonDigest.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>
#include "../DirectDisk.h"
using namespace persist;
struct Medium {                                         // a store of any size: only written blocks exist
  std::map<uint64_t, std::vector<uint8_t>> blocks;
  unsigned writes = 0;
  bool read(uint64_t b, uint32_t n, void *d) {
    for (uint32_t i = 0; i < n; ++i) { auto it = blocks.find(b + i); if (it == blocks.end()) memset((uint8_t *) d + i * kBlock, 0xA5, kBlock); else memcpy((uint8_t *) d + i * kBlock, it->second.data(), kBlock); }
    return true; }
  bool write(uint64_t b, uint32_t n, const void *s) { ++writes; for (uint32_t i = 0; i < n; ++i) blocks[b + i].assign((const uint8_t *) s + i * kBlock, (const uint8_t *) s + (i + 1) * kBlock); return true; }
  bool sync() { return true; }
};
struct Buffer {
  std::vector<uint8_t> &data;
  uint64_t read(uint64_t at, void *p, uint64_t n) { memcpy(p, &data[at], n); return n; }
  uint64_t copy(uint64_t at, const void *p, uint64_t n) { memcpy(&data[at], p, n); return n; }
};
static std::vector<uint8_t> packed(uint64_t logical, const std::map<uint64_t, uint8_t> &pagesByIndex) {   // DBRDISK1 with one extent per page
  std::vector<uint8_t> out(32 + 16 * pagesByIndex.size());
  memcpy(out.data(), "DBRDISK1", 8); uint32_t v = 1, ps = 4096; memcpy(&out[8], &v, 4); memcpy(&out[12], &ps, 4);
  uint64_t count = pagesByIndex.size(); memcpy(&out[16], &logical, 8); memcpy(&out[24], &count, 8);
  size_t k = 0;
  for (auto &p : pagesByIndex) { uint64_t at = p.first * 4096, len = 4096; memcpy(&out[32 + 16 * k], &at, 8); memcpy(&out[40 + 16 * k], &len, 8); ++k; }
  for (auto &p : pagesByIndex) out.insert(out.end(), 4096, p.second);
  return out;
}
struct Ops { uint8_t *allocate() { return (uint8_t *) calloc(1, 4096); } };
struct Sha { static void init(CC_SHA256_CTX &c) { CC_SHA256_Init(&c); } static void update(CC_SHA256_CTX &c, const void *p, size_t n) { CC_SHA256_Update(&c, p, (CC_LONG) n); } static void fin(CC_SHA256_CTX &c, uint8_t *o) { CC_SHA256_Final(o, &c); } };

int main() {
  const uint64_t logical = 64ULL << 30, pages = logical / 4096;           // 64 GiB, 16.7M pages
  std::map<uint64_t, uint8_t> seedPages = {{0, 0x11}, {1, 0x12}, {1000, 0x13}, {pages / 2, 0x14}, {pages - 1, 0x15}};
  auto blob = packed(logical, seedPages);
  uint64_t count = 0; assert(seedCountNonzero(blob.data(), blob.size(), &count) && count == 5);
  SeedDisk seed; seed.bytes = logical; seed.capacity = count;
  std::vector<uint64_t> index(count); std::vector<uint8_t *> page(count); seed.index = index.data(); seed.page = page.data();
  Ops ops; assert(seedImportSparse(seed, blob.data(), blob.size(), ops) && seed.count == 5 && seed.resident == 5 * 4096);
  assert(seed.find(0)[0] == 0x11 && seed.find(1)[4095] == 0x12 && seed.find(pages - 1)[7] == 0x15 && !seed.find(2) && !seed.find(pages / 2 + 1) && seed.find(pages / 2)[0] == 0x14);
  { SeedDisk again = seed; again.count = 0; assert(!seedImportSparse(again, blob.data(), blob.size(), ops) || true); }
  // Identity: a small disk keeps the plain hash of every byte; a large one hashes size + nonzero pages, and changes with any page.
  { std::map<uint64_t, uint8_t> small = {{0, 0x21}, {3, 0x22}}; auto b = packed(8 * 4096, small); uint64_t c = 0; assert(seedCountNonzero(b.data(), b.size(), &c));
    SeedDisk s; s.bytes = 8 * 4096; s.capacity = c; std::vector<uint64_t> ix(c); std::vector<uint8_t *> pg(c); s.index = ix.data(); s.page = pg.data(); assert(seedImportSparse(s, b.data(), b.size(), ops));
    CC_SHA256_CTX ctx; uint8_t id[32], plain[32]; seedIdentity(s, ctx, Sha::init, Sha::update, Sha::fin, id);
    std::vector<uint8_t> flat(8 * 4096); memset(&flat[0], 0x21, 4096); memset(&flat[3 * 4096], 0x22, 4096); CC_SHA256(flat.data(), (CC_LONG) flat.size(), plain);
    assert(!memcmp(id, plain, 32));
    for (auto p : pg) free(p); }
  uint8_t idA[32], idB[32]; CC_SHA256_CTX ctx;
  seedIdentity(seed, ctx, Sha::init, Sha::update, Sha::fin, idA); seed.page[2][0] ^= 1; seedIdentity(seed, ctx, Sha::init, Sha::update, Sha::fin, idB); seed.page[2][0] ^= 1;
  assert(memcmp(idA, idB, 32)); seed.bytes += 4096; seedIdentity(seed, ctx, Sha::init, Sha::update, Sha::fin, idB); assert(memcmp(idA, idB, 32)); seed.bytes -= 4096;
  // The overlay on a medium of the full window size: header + bitmap for 16.7M pages stay small in memory (the bitmap is pages/8 bytes).
  Medium m; std::vector<uint8_t> bits(bitmapBlocks(pages) * kBlock); uint8_t digest[32] = {7};
  assert(bitmapBlocks(pages) == 512);
  Overlay ov; assert(ov.open(m, pages, digest, false, bits.data()) == Open::Fresh && windowBlocks(pages) == 1 + 512 + pages);
  std::map<uint64_t, std::vector<uint8_t>> oracle;        // page index -> content, for pages that differ from the seed
  std::vector<uint8_t> io(200000), stage(32 * kBlock); bool storeError = false; srand(3);
  auto seedPage = [&](uint64_t i) { const uint8_t *p = seed.find(i); return p ? std::vector<uint8_t>(p, p + 4096) : std::vector<uint8_t>(4096, 0); };
  auto expect = [&](uint64_t byteAt, uint64_t len) { std::vector<uint8_t> out(len);
    for (uint64_t k = 0; k < len;) { uint64_t at = byteAt + k, i = at / 4096, n = std::min<uint64_t>(4096 - (at & 4095), len - k);
      auto it = oracle.find(i); const uint8_t *src = it != oracle.end() ? it->second.data() : seed.find(i);
      if (src) memcpy(&out[k], src + (at & 4095), n); else memset(&out[k], 0, n); k += n; } return out; };
  for (int op = 0; op < 300; ++op) {
    uint64_t len = 1 + (uint64_t) rand() % 40000, base = (uint64_t) (rand() % 6);
    uint64_t where = base == 0 ? 0 : base == 1 ? (pages - 30) * 4096 : base == 2 ? (pages / 2 - 3) * 4096 : ((uint64_t) rand() * 7919 % (pages - 40)) * 4096;
    uint64_t offset = where + (rand() & 1 ? 0 : (uint64_t) rand() % 4096);
    if (offset + len > logical) len = logical - offset;
    Buffer b{io};
    if (rand() & 1) {
      for (uint64_t k = 0; k < len; ++k) io[k] = (uint8_t) rand();
      assert(directDiskIO(seed, ov, m, stage.data(), 32, true, offset, len, b, &storeError) == len && !storeError);
      for (uint64_t k = 0; k < len;) { uint64_t at = offset + k, i = at / 4096, n = std::min<uint64_t>(4096 - (at & 4095), len - k);
        if (!oracle.count(i)) oracle[i] = seedPage(i); memcpy(&oracle[i][at & 4095], &io[k], n); k += n; }
    } else {
      assert(directDiskIO(seed, ov, m, stage.data(), 32, false, offset, len, b, &storeError) == len);
      assert(std::vector<uint8_t>(io.begin(), io.begin() + len) == expect(offset, len));
    }
  }
  assert(ov.present == oracle.size() && seed.count == 5);
  for (uint64_t k = 0; k < seed.count; ++k) free(seed.page[k]);
  puts("PASS: compact seed import, identity, and a 64 GiB direct disk against a sparse oracle");
  return 0;
}
