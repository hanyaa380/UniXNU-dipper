// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../SparseDisk.h"
#include <assert.h>
#include <stdlib.h>
#include <vector>
#include <stdio.h>
struct Ops {
 std::vector<uint8_t> buffer;long budget=-1;uint64_t shortAt=UINT64_MAX;
 uint8_t *allocate(){if(budget==0)return nullptr;if(budget>0)--budget;return static_cast<uint8_t *>(calloc(1,4096));}
 uint64_t read(uint64_t at,void *p,uint64_t n){if(at>=shortAt)return 0;if(n>shortAt-at)n=shortAt-at;memcpy(p,buffer.data()+at,n);return n;}
 uint64_t copy(uint64_t at,const void *p,uint64_t n){if(at>=shortAt)return 0;if(n>shortAt-at)n=shortAt-at;memcpy(buffer.data()+at,p,n);return n;}
};
static void put32(std::vector<uint8_t>&b,size_t at,uint32_t v){memcpy(b.data()+at,&v,4);}
static void put64(std::vector<uint8_t>&b,size_t at,uint64_t v){memcpy(b.data()+at,&v,8);}
static void cleanup(RamSparseDisk &d){for(uint64_t i=0;i<d.bytes/4096;++i)free(d.pages[i]);free(d.pages);d={};}
int main(){
 const uint64_t size=4096*32;std::vector<uint8_t> packed(48+4096);
 memcpy(packed.data(),"DBRDISK1",8);put32(packed,8,1);put32(packed,12,4096);put64(packed,16,size);put64(packed,24,1);put64(packed,32,4096*7);put64(packed,40,4096);packed[48+9]=0xa5;
 uint64_t logical=0;assert(ramPackedLayout(packed.data(),packed.size(),&logical)&&logical==size);
 for(unsigned which=0;which<9;++which){auto bad=packed;
  if(which==0)bad[0]=0;
  if(which==1)put32(bad,8,2);
  if(which==2)put32(bad,12,16384);
  if(which==3)put64(bad,16,0x80001000);
  if(which==4)put64(bad,24,65537);
  if(which==5)put64(bad,32,1);
  if(which==6)put64(bad,40,0);
  if(which==7)bad.pop_back();
  if(which==8)bad.push_back(0);
  assert(!ramPackedLayout(bad.data(),bad.size(),&logical));
 }
 Ops op;RamSparseDisk d={static_cast<uint8_t **>(calloc(size/4096,sizeof(uint8_t*))),size,0};
 assert(ramSparseImport(d,packed.data(),packed.size(),op)&&d.resident==4096);
 std::vector<uint8_t> flat(size);flat[7*4096+9]=0xa5;uint64_t random=42;
 for(unsigned i=0;i<4000;++i){random=random*6364136223846793005ULL+1;uint64_t at=random%size;
  uint64_t n=1+((random>>32)%9000);if(n>size-at)n=size-at;
  op.buffer.assign(n,0);bool write=(i%3)!=0;
  if(write)for(size_t j=0;j<n;++j)op.buffer[j]=static_cast<uint8_t>(i+j);
  bool oom=false;assert(ramSparseIO(d,write,at,n,op,oom)==n&&!oom);
  if(write)memcpy(flat.data()+at,op.buffer.data(),n);else assert(!memcmp(flat.data()+at,op.buffer.data(),n));
 }
 op.buffer.assign(size,0);bool oom=false;assert(ramSparseIO(d,false,0,size,op,oom)==size && op.buffer==flat);
 assert(ramSparseIO(d,false,UINT64_MAX,1,op,oom)==0);assert(ramSparseIO(d,true,size,1,op,oom)==0);cleanup(d);
 d={static_cast<uint8_t **>(calloc(2,sizeof(uint8_t*))),8192,0};op.buffer.assign(8192,0x37);op.budget=1;
 assert(ramSparseIO(d,true,0,8192,op,oom)==4096&&oom&&d.resident==4096&&!d.pages[1]);
 op.buffer.assign(8192,0);op.budget=-1;assert(ramSparseIO(d,false,0,8192,op,oom)==8192&&!oom);
 for(size_t i=0;i<8192;++i)assert(op.buffer[i]==(i<4096?0x37:0));
 op.shortAt=100;op.buffer.assign(4096,0x55);assert(ramSparseIO(d,true,0,4096,op,oom)==100&&!oom);
 assert(d.pages[0][99]==0x55&&d.pages[0][100]==0x37);cleanup(d);
 d={static_cast<uint8_t **>(calloc(size/4096,sizeof(uint8_t*))),size,0};op.budget=0;
 assert(!ramSparseImport(d,packed.data(),packed.size(),op)&&d.resident==0);cleanup(d);
 puts("PASS: actual sparse parser/import/I/O, 4000 differential operations, overflow/malformed/short-copy/allocation failures");
}
