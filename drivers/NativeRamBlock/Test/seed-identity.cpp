// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host-only: the identity SeedDisk.h computes for a packed seed file, to compare with drivers/DarwinBoot/Test/packed_ramdisk.py.
#include <CommonCrypto/CommonDigest.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "../SeedDisk.h"
struct Ops { uint8_t *allocate() { return static_cast<uint8_t *>(calloc(1, 4096)); } };
int main(int argc, char **argv) {
  assert(argc == 2);
  FILE *f = fopen(argv[1], "rb"); assert(f); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
  std::vector<uint8_t> in(n); assert(fread(in.data(), 1, n, f) == (size_t) n); fclose(f);
  uint64_t logical = 0, count = 0; assert(ramPackedLayout(in.data(), in.size(), &logical) && seedCountNonzero(in.data(), in.size(), &count));
  SeedDisk d; d.bytes = logical; d.capacity = count; std::vector<uint64_t> ix(count ? count : 1); std::vector<uint8_t *> pg(count ? count : 1); d.index = ix.data(); d.page = pg.data();
  Ops ops; assert(seedImportSparse(d, in.data(), in.size(), ops));
  CC_SHA256_CTX ctx; uint8_t out[32];
  seedIdentity(d, ctx, [](CC_SHA256_CTX &c) { CC_SHA256_Init(&c); }, [](CC_SHA256_CTX &c, const void *p, size_t k) { CC_SHA256_Update(&c, p, (CC_LONG) k); },
               [](CC_SHA256_CTX &c, uint8_t *o) { CC_SHA256_Final(o, &c); }, out);
  for (uint8_t c : out) printf("%02x", c);
  printf("\n");
}
