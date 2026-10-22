// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
struct RamSparseDisk {
  uint8_t **pages;
  uint64_t bytes, resident;
  const uint8_t *find(uint64_t i) const { return pages ? pages[i] : nullptr; }
};
static uint32_t ramU32(const uint8_t *p){uint32_t v;memcpy(&v,p,4);return v;}
static uint64_t ramU64(const uint8_t *p){uint64_t v;memcpy(&v,p,8);return v;}
static bool ramPackedLayout(const uint8_t *p,uint64_t n,uint64_t *logical) {
  if(!p || n<32 || n>0x10000000 || memcmp(p,"DBRDISK1",8) || ramU32(p+8)!=1 || ramU32(p+12)!=4096)return false;
  uint64_t size=ramU64(p+16),count=ramU64(p+24),end=0;
  if(!size || size>(1ULL<<40) || (size&4095) || count>65536 || count>(n-32)/16)return false;
  uint64_t cursor=32+count*16;
  for(uint64_t i=0;i<count;++i){
    uint64_t at=ramU64(p+32+i*16),len=ramU64(p+40+i*16);
    if(!len || ((at|len)&4095) || at<end || at>size || len>size-at || len>n-cursor)return false;
    end=at+len;cursor+=len;
  }
  if(cursor!=n)return false;
  *logical=size;return true;
}
template<class Ops> static bool ramSparseImport(RamSparseDisk &d,const uint8_t *p,uint64_t n,Ops &ops) {
  uint64_t size=0;if(!d.pages || !ramPackedLayout(p,n,&size) || d.bytes!=size || d.resident)return false;
  uint64_t count=ramU64(p+24),cursor=32+count*16;
  for(uint64_t i=0;i<count;++i){
    uint64_t at=ramU64(p+32+i*16),len=ramU64(p+40+i*16);
    for(uint64_t j=0;j<len;j+=4096){
      const uint8_t *src=p+cursor+j;uint8_t any=0;
      for(unsigned k=0;k<4096;++k)any|=src[k];
      if(!any)continue;
      uint8_t *page=ops.allocate();if(!page)return false;
      memcpy(page,src,4096);d.pages[(at+j)/4096]=page;d.resident+=4096;
    }
    cursor+=len;
  }
  return true;
}
// Caller serializes I/O. Ops performs genuine descriptor reads/writes and
// zero-initialized allocation. Short copies preserve truthful byte counts.
template<class Ops> static uint64_t ramSparseIO(RamSparseDisk &d,bool write,uint64_t offset,uint64_t length,Ops &ops,bool &noMemory) {
  static const uint8_t zero[4096]={};noMemory=false;
  if(!d.pages || !length || offset>d.bytes || length>d.bytes-offset)return 0;
  uint64_t done=0;
  while(done<length){
    uint64_t at=offset+done,index=at/4096,skip=at&4095,n=4096-skip;
    if(n>length-done)n=length-done;
    uint8_t *page=d.pages[index];
    if(write && !page){
      page=ops.allocate();if(!page){noMemory=true;break;}
      d.pages[index]=page;d.resident+=4096;
    }
    uint64_t actual=write?ops.read(done,page+skip,n):ops.copy(done,(page?page:zero)+skip,n);
    if(actual>n)break;
    done+=actual;if(actual!=n)break;
  }
  return done;
}
