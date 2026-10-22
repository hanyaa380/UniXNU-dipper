// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host-only complete logical-seed check without materializing a dense disk.
#include "../SparseDisk.h"
#include <CommonCrypto/CommonDigest.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <vector>
int main(int argc,char **argv){
 assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);assert(fseek(f,0,SEEK_END)==0);long n=ftell(f);assert(n>=32&&n<=0x10000000);rewind(f);
 std::vector<uint8_t> input(n);assert(fread(input.data(),1,n,f)==static_cast<size_t>(n));fclose(f);
 uint64_t logical=0;assert(ramPackedLayout(input.data(),input.size(),&logical));
 RamSparseDisk d={static_cast<uint8_t **>(calloc(logical/4096,sizeof(uint8_t*))),logical,0};
 struct Ops{uint8_t *allocate(){return static_cast<uint8_t *>(calloc(1,4096));}} op;
 assert(ramSparseImport(d,input.data(),input.size(),op));
 CC_SHA256_CTX context;CC_SHA256_Init(&context);const uint8_t zero[4096]={};uint8_t digest[32];
 for(uint64_t i=0;i<logical/4096;++i)CC_SHA256_Update(&context,d.pages[i]?d.pages[i]:zero,4096);
 CC_SHA256_Final(digest,&context);for(uint8_t c:digest)printf("%02x",c);printf(" %llu %llu\n",(unsigned long long)d.bytes,(unsigned long long)d.resident);
 for(uint64_t i=0;i<logical/4096;++i)free(d.pages[i]);free(d.pages);
}
