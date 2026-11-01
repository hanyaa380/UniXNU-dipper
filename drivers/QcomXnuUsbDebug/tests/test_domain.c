/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "soc/QcomUsbDmaDomain.h"

struct cell {QUSB_U64 a,v;};
struct model {
  struct cell cells[256],initial[256]; unsigned int count,initial_count,writes,fail_on,after,cleaned;
  QUSB_DOMAIN_LEASE *lease;QUSB_U64 *tables;QUSB_U64 table_pa,dma_pa;
  unsigned int ext,timeout,delay;
  QUSB_U32 forced_a64, corrupt_tcr2;
  unsigned int fail_capture, fail_tcr2_readback;
  QUSB_U32 fsr_on_enable;
  QUSB_U32 fsr_format_override;
};
static QUSB_U32 model_forced_a64;
static void set(struct model*m,QUSB_U64 a,QUSB_U64 v)
{
  unsigned int i;for(i=0;i<m->count;i++)if(m->cells[i].a==a){m->cells[i].v=v;return;}
  assert(m->count<256);m->cells[m->count++]=(struct cell){a,v};
}
static QUSB_U64 rawget(struct model*m,QUSB_U64 a)
{
  unsigned int i;for(i=0;i<m->count;i++)if(m->cells[i].a==a)return m->cells[i].v;return 0;
}
static QUSB_U64 get(struct model*m,QUSB_U64 a)
{
  QUSB_U64 v=rawget(m,a),cb;
  if(m->forced_a64&&a>=QUSB_SMMU_BASE+0x80000&&a<QUSB_SMMU_BASE+0x84000&&
     ((a-QUSB_SMMU_BASE)&4095U)==0x10U) {
    cb=(a-QUSB_SMMU_BASE-0x80000)/4096;
    if(rawget(m,QUSB_SMMU_BASE+0x1800+4*cb)&1U)v|=m->forced_a64;
    else v&=~(QUSB_U64)m->forced_a64;
  }
  if(a>=QUSB_SMMU_BASE+0x80000&&a<QUSB_SMMU_BASE+0x84000&&
     ((a-QUSB_SMMU_BASE)&4095U)==0x58U) {
    cb=(a-QUSB_SMMU_BASE-0x80000)/4096;
    v&=~0x600ULL;
    if(rawget(m,QUSB_SMMU_BASE+0x1800+4*cb)&1U)v|=0x400U;
    else if(rawget(m,QUSB_SMMU_BASE+0x80030+0x1000*cb)&0x80000000U)v|=0x200U;
    if(m->fsr_format_override&&(rawget(m,QUSB_SMMU_BASE+0x80000+0x1000*cb)&1U))
      v=(v&~0x600U)|m->fsr_format_override;
  }
  return v;
}
static int r32(void*c,QUSB_U64 a,QUSB_U32*v)
{
  struct model*m=c;assert(!(a&3U));
  if(m->fail_capture&&m->lease->active&&m->lease->tcr2_capture_pending&&
     a==m->lease->regs[2].address){m->fail_capture=0;return 0;}
  if(m->fail_tcr2_readback&&m->lease->active&&m->lease->phase==1&&m->lease->operation==2&&
     a==m->lease->regs[2].address){m->fail_tcr2_readback=0;return 0;}
  if(m->timeout&&m->lease->active&&a==m->lease->cb_base+0x7f4){*v=1;return 1;}
  *v=(QUSB_U32)get(m,a);return 1;
}
static int r64(void*c,QUSB_U64 a,QUSB_U64*v) {assert(!(a&7U));*v=get(c,a);return 1;}
static QUSB_U64 walk(struct model*m,QUSB_U64 iova)
{
  QUSB_U64 entry;QUSB_U32 level,indices[3]={(QUSB_U32)((iova>>30)&511U),(QUSB_U32)((iova>>21)&511U),(QUSB_U32)((iova>>12)&511U)};
  QUSB_U64 table=m->table_pa;
  if(iova>>32)return 0;
  for(level=0;level<3;level++) {
    assert(table>=m->table_pa&&table+4096<=m->table_pa+QUSB_DOMAIN_PT_BYTES);
    entry=m->tables[(table-m->table_pa)/8+indices[level]];
    if(!(entry&1U))return 0;
    assert((entry&3U)==3U);
    if(level<2){table=entry&0x0000fffffffff000ULL;continue;}
    assert((entry&0x0060000000000fc0ULL)==0x0060000000000f40ULL); /* XN,nG,AF,SH_IS,AP=RW. */
    return (entry&0x0000fffffffff000ULL)|(iova&4095U);
  }
  assert(0);return 0;
}
static int writer(struct model*m,QUSB_U64 a,QUSB_U64 v,QUSB_U32 width)
{
  unsigned int i,allowed=0,fail;
  QUSB_DOMAIN_LEASE*l=m->lease;
  assert(QcomUsbDomainWriteAllowed(l,a,v,width));
  assert(a!=QUSB_SMMU_BASE&&a!=QUSB_SMMU_BASE+0x68&&a!=QUSB_SMMU_BASE+0x70);
  for(i=0;i<QUSB_DOMAIN_REGISTER_COUNT;i++)if(a==l->regs[i].address)allowed=1;
  if(a==l->cb_base+0x610||a==l->cb_base+0x7f0||
     a==QUSB_SMMU_BASE+0x800+4*l->index||a==QUSB_SMMU_BASE+0xc00+4*l->index)allowed=1;
  assert(allowed);
  if(a==l->cb_base&&v&1U) {
    assert(m->cleaned&&walk(m,QUSB_DOMAIN_IOVA)==m->dma_pa);
    assert((v&(1U<<6))==0); /* IRQ delivery remains disabled. */
  }
  fail=(++m->writes==m->fail_on);
  if(!fail||m->after) {
    if(a==l->regs[2].address) {
      if(get(m,l->regs[0].address)&1U)assert((v&m->forced_a64)==m->forced_a64);
      v^=m->corrupt_tcr2;v&=~(QUSB_U64)m->forced_a64;
    }
    if(a!=l->cb_base+0x610&&a!=l->cb_base+0x7f0)set(m,a,v);
    if(a==l->cb_base&&(v&1U)&&m->fsr_on_enable)set(m,l->cb_base+0x58,m->fsr_on_enable);
  }
  return !fail;
}
static int w32(void*c,QUSB_U64 a,QUSB_U32 v){return writer(c,a,v,4);}
static int w64(void*c,QUSB_U64 a,QUSB_U64 v){return writer(c,a,v,8);}
static void barrier(void*c){(void)c;}
static void delay(void*c,QUSB_U32 us){((struct model*)c)->delay+=us;}
static int clean(void*c,void*cpu,QUSB_U32 bytes)
{
  struct model*m=c;assert(cpu==m->tables&&bytes==QUSB_DOMAIN_PT_BYTES);m->cleaned=1;return 1;
}
static QUSB_DOMAIN_PLATFORM platform(struct model*m)
{
  QUSB_DOMAIN_PLATFORM p={m,r32,w32,r64,w64,barrier,delay,clean};return p;
}
static void init(struct model*m,QUSB_DOMAIN_LEASE*l,void*pt,unsigned int ext)
{
  unsigned int cb;QUSB_U64 base;
  memset(m,0,sizeof(*m));memset(l,0,sizeof(*l));m->lease=l;m->tables=pt;m->ext=ext;
  m->forced_a64=model_forced_a64;
  m->table_pa=0x123450000ULL;m->dma_pa=0x234560000ULL;
  set(m,QUSB_DWC3_BASE+0xc110,2U<<12);set(m,QUSB_DWC3_BASE+0xc70c,1U<<22);
  set(m,QUSB_SMMU_BASE+0x20,(1U<<30)|(1U<<27)|(ext<<8)|4U);
  set(m,QUSB_SMMU_BASE+0x24,(6U<<28)|(1U<<16)|4U); /* 128 global pages, 4 CBs, CB0 S2-only. */
  set(m,QUSB_SMMU_BASE+0x28,(1U<<12)|0x44U);set(m,QUSB_SMMU_BASE,(1U<<10)|(ext<<3));
  set(m,QUSB_SMMU_BASE+0x800,ext?0x80007777U:0x12347777U);
  set(m,QUSB_SMMU_BASE+0xc00,0x20000U); /* Original invalid slot. */
  set(m,QUSB_SMMU_BASE+0x804,(ext?0:0x80000000U)|0x100U);
  set(m,QUSB_SMMU_BASE+0xc04,1U|(ext<<10)); /* Other stream owns CB1. */
  for(cb=0;cb<4;cb++) {
    base=QUSB_SMMU_BASE+0x80000+0x1000*cb;
    set(m,base+0x20,((QUSB_U64)0xff00U<<48)|(0x10000000ULL+cb*4096));
    set(m,base+0x28,((QUSB_U64)(0x200U+cb)<<48));
    set(m,base,0x1000U); /* Inactive but nonzero attributes must restore exactly. */
  }
  set(m,QUSB_SMMU_BASE+0x1000+4*3,(3U<<16)|(2U<<8)); /* Inactive CB3 references CB2 conservatively. */
  m->initial_count=m->count;memcpy(m->initial,m->cells,sizeof(m->cells));
}
static void restored(struct model*m)
{
  unsigned int i;assert(!m->lease->active);
  for(i=0;i<m->initial_count;i++)assert(get(m,m->initial[i].a)==m->initial[i].v);
}
int main(void)
{
  void*pt=0;struct model m;QUSB_DOMAIN_LEASE l,bad;QUSB_DOMAIN_PLATFORM p;
  QUSB_DOMAIN_RESULT result;unsigned int extra,ext,i,fail,after,install_writes,restore_writes,before;
  static const QUSB_U32 fault_bits[]={2,4,8,0x10,0x20,0x40,0x80,0x100,0x40000000U,0x80000000U,1,0x800};
  assert(!posix_memalign(&pt,4096,QUSB_DOMAIN_PT_BYTES));
  for(extra=0;extra<2;extra++) {model_forced_a64=extra?0x80000520U:0;
  for(ext=0;ext<2;ext++) {
    init(&m,&l,pt,ext);p=platform(&m);
    result=QcomUsbDomainInstall(&p,pt,m.table_pa,QUSB_DOMAIN_PT_BYTES,m.dma_pa,QUSB_DOMAIN_DMA_BYTES,&l);
    assert(result==QusbDomainOkay&&l.active&&l.cb==3&&l.index==0&&l.asid!=0xff00U);
    assert(get(&m,l.cb_base+0x58)==0x400U);
    assert(l.tcr2_preserved==m.forced_a64&&
      l.regs[2].installed==((QUSB_U64)0x38014U|m.forced_a64));
    install_writes=m.writes;assert(install_writes==15);
    for(i=0;i<8;i++)assert(walk(&m,QUSB_DOMAIN_IOVA+4096*i)==m.dma_pa+4096*i);
    assert(walk(&m,QUSB_DOMAIN_IOVA+32767)==m.dma_pa+32767);
    assert(!walk(&m,QUSB_DOMAIN_IOVA-1)&&!walk(&m,QUSB_DOMAIN_IOVA+32768));
    assert(!walk(&m,0)&&!walk(&m,0xffffffffU)&&!walk(&m,0x190000000ULL));
    assert(!QcomUsbDomainWriteAllowed(&l,QUSB_SMMU_BASE,0,4));
    assert(!QcomUsbDomainWriteAllowed(&l,l.cb_base,0,4));
    bad=l;bad.phase=4;assert(QcomUsbDomainWriteAllowed(&bad,bad.cb_base,0x27,4));
    assert(!QcomUsbDomainWriteAllowed(&bad,bad.cb_base,0x26,4));
    assert(!QcomUsbDomainWriteAllowed(&bad,bad.cb_base,0x1027,4));
    bad.cb=0;assert(!QcomUsbDomainWriteAllowed(&bad,bad.cb_base,0x27,4));
    bad=l;bad.phase=1;bad.operation=2;bad.tcr2_view_owned=1;
    assert(QcomUsbDomainWriteAllowed(&bad,bad.regs[2].address,bad.regs[2].installed,4));
    assert(!QcomUsbDomainWriteAllowed(&bad,bad.regs[2].address,bad.regs[2].installed^(1U<<20),4));
    bad.tcr2_bound=0;
    assert(!QcomUsbDomainWriteAllowed(&bad,bad.regs[2].address,bad.regs[2].installed,4));
    bad.tcr2_bound=1;bad.tcr2_observed^=1U<<20;
    assert(!QcomUsbDomainWriteAllowed(&bad,bad.regs[2].address,bad.regs[2].installed,4));
    assert(QcomUsbDomainRestore(&p,&l)==QusbDomainOkay);restore_writes=m.writes-install_writes;restored(&m);
    for(fail=1;fail<=install_writes;fail++)for(after=0;after<2;after++) {
      init(&m,&l,pt,ext);p=platform(&m);m.fail_on=fail;m.after=after;
      result=QcomUsbDomainInstall(&p,pt,m.table_pa,QUSB_DOMAIN_PT_BYTES,m.dma_pa,QUSB_DOMAIN_DMA_BYTES,&l);
      assert(result!=QusbDomainOkay&&l.active);m.fail_on=0;before=m.writes;
      result=QcomUsbDomainRestore(&p,&l);
      if(l.tlb_poisoned){assert(result==QusbDomainTlbTimeout&&m.writes==before&&l.active);}
      else {assert(result==QusbDomainOkay);restored(&m);}
    }
    for(fail=1;fail<=restore_writes;fail++)for(after=0;after<2;after++) {
      init(&m,&l,pt,ext);p=platform(&m);
      assert(QcomUsbDomainInstall(&p,pt,m.table_pa,QUSB_DOMAIN_PT_BYTES,m.dma_pa,QUSB_DOMAIN_DMA_BYTES,&l)==QusbDomainOkay);
      m.fail_on=m.writes+fail;m.after=after;
      assert(QcomUsbDomainRestore(&p,&l)!=QusbDomainOkay&&l.active);m.fail_on=0;before=m.writes;
      result=QcomUsbDomainRestore(&p,&l);
      if(l.tlb_poisoned){assert(result==QusbDomainTlbTimeout&&m.writes==before);}
      else {assert(result==QusbDomainOkay);restored(&m);}
    }
    init(&m,&l,pt,ext);p=platform(&m);m.timeout=1;
    assert(QcomUsbDomainInstall(&p,pt,m.table_pa,QUSB_DOMAIN_PT_BYTES,m.dma_pa,QUSB_DOMAIN_DMA_BYTES,&l)==QusbDomainTlbTimeout);
    before=m.writes;assert(l.active&&l.tlb_poisoned&&m.delay==1000);
    assert(QcomUsbDomainRestore(&p,&l)==QusbDomainTlbTimeout&&m.writes==before);
    init(&m,&l,pt,ext);p=platform(&m);
    set(&m,QUSB_SMMU_BASE+0x808,(ext?0:0x80000000U)|QUSB_STREAM_ID);
    set(&m,QUSB_SMMU_BASE+0xc08,2U|(ext<<10));
    assert(QcomUsbDomainInstall(&p,pt,m.table_pa,QUSB_DOMAIN_PT_BYTES,m.dma_pa,QUSB_DOMAIN_DMA_BYTES,&l)==QusbDomainStreamExists);
    assert(!m.writes&&!l.active);
    init(&m,&l,pt,ext);p=platform(&m);set(&m,QUSB_SMMU_BASE+0x83000,1U);
    assert(QcomUsbDomainInstall(&p,pt,m.table_pa,QUSB_DOMAIN_PT_BYTES,m.dma_pa,QUSB_DOMAIN_DMA_BYTES,&l)==QusbDomainNoBank);
    assert(!m.writes&&!l.active);
    init(&m,&l,pt,ext);p=platform(&m);m.fail_capture=1;
    assert(QcomUsbDomainInstall(&p,pt,m.table_pa,QUSB_DOMAIN_PT_BYTES,m.dma_pa,QUSB_DOMAIN_DMA_BYTES,&l)==QusbDomainReadFailed);
    assert(l.active&&l.tcr2_capture_pending);
    assert(QcomUsbDomainRestore(&p,&l)==QusbDomainOkay);restored(&m);
    init(&m,&l,pt,ext);p=platform(&m);m.fsr_format_override=0x200U;
    assert(QcomUsbDomainInstall(&p,pt,m.table_pa,QUSB_DOMAIN_PT_BYTES,m.dma_pa,QUSB_DOMAIN_DMA_BYTES,&l)==QusbDomainOwnershipChanged);
    assert(l.active&&l.enable_attempted&&l.phase==6);
    init(&m,&l,pt,ext);p=platform(&m);m.fail_tcr2_readback=1;
    assert(QcomUsbDomainInstall(&p,pt,m.table_pa,QUSB_DOMAIN_PT_BYTES,m.dma_pa,QUSB_DOMAIN_DMA_BYTES,&l)==QusbDomainReadFailed);
    assert(QcomUsbDomainRestore(&p,&l)==QusbDomainOkay);restored(&m);
    for(i=0;i<3;i++) {
      init(&m,&l,pt,ext);p=platform(&m);m.corrupt_tcr2=i==0?0x10U:(i==1?0x8000U:1U);
      assert(QcomUsbDomainInstall(&p,pt,m.table_pa,QUSB_DOMAIN_PT_BYTES,m.dma_pa,QUSB_DOMAIN_DMA_BYTES,&l)==QusbDomainWriteFailed);
      assert(l.active&&!l.enable_attempted);before=m.writes;
      assert(QcomUsbDomainRestore(&p,&l)==QusbDomainOwnershipChanged&&m.writes==before);
    }
    init(&m,&l,pt,ext);p=platform(&m);
    assert(QcomUsbDomainInstall(&p,pt,m.table_pa,QUSB_DOMAIN_PT_BYTES,m.dma_pa,QUSB_DOMAIN_DMA_BYTES,&l)==QusbDomainOkay);
    set(&m,l.regs[2].address,rawget(&m,l.regs[2].address)^(1U<<20));before=m.writes;
    assert(QcomUsbDomainRestore(&p,&l)==QusbDomainOwnershipChanged&&m.writes==before);
    init(&m,&l,pt,ext);p=platform(&m);
    assert(QcomUsbDomainInstall(&p,pt,m.table_pa,QUSB_DOMAIN_PT_BYTES,m.dma_pa,QUSB_DOMAIN_DMA_BYTES,&l)==QusbDomainOkay);
    m.fail_on=m.writes+1;
    assert(QcomUsbDomainRestore(&p,&l)==QusbDomainWriteFailed);
    m.fail_on=0;set(&m,l.regs[2].address,l.regs[2].saved);before=m.writes;
    assert(QcomUsbDomainRestore(&p,&l)==QusbDomainOwnershipChanged&&m.writes==before);
    init(&m,&l,pt,ext);p=platform(&m);
    set(&m,QUSB_SMMU_BASE+0x1000+4,1U<<16); /* Other NS stage1 bank. */
    set(&m,QUSB_SMMU_BASE+0x81020,((QUSB_U64)0xfeffU<<48)|0x10001000U);
    assert(QcomUsbDomainInstall(&p,pt,m.table_pa,QUSB_DOMAIN_PT_BYTES,m.dma_pa,QUSB_DOMAIN_DMA_BYTES,&l)==QusbDomainOkay);
    assert(l.asid!=0xff00U&&l.asid!=0xfeffU&&get(&m,l.cb_base)==0x27U);
    assert(QcomUsbDomainRestore(&p,&l)==QusbDomainOkay);
    for(i=0;i<sizeof(fault_bits)/sizeof(fault_bits[0]);i++) {
      init(&m,&l,pt,ext);p=platform(&m);m.fsr_on_enable=fault_bits[i];
      assert(QcomUsbDomainInstall(&p,pt,m.table_pa,QUSB_DOMAIN_PT_BYTES,m.dma_pa,QUSB_DOMAIN_DMA_BYTES,&l)==QusbDomainOwnershipChanged);
      assert(l.active&&l.enable_attempted&&l.phase==6);before=m.writes;
      assert(QcomUsbDomainRestore(&p,&l)==QusbDomainOwnershipChanged&&m.writes==before);
      init(&m,&l,pt,ext);p=platform(&m);set(&m,QUSB_SMMU_BASE+0x83058,fault_bits[i]);
      assert(QcomUsbDomainInstall(&p,pt,m.table_pa,QUSB_DOMAIN_PT_BYTES,m.dma_pa,QUSB_DOMAIN_DMA_BYTES,&l)==QusbDomainNoBank);
      assert(!m.writes&&!l.active);
    }
    init(&m,&l,pt,ext);p=platform(&m);
    set(&m,QUSB_SMMU_BASE+0x1800+4*3,1U); /* Already-A64 idle bank reports FORMAT2, not fault. */
    m.initial_count=m.count;memcpy(m.initial,m.cells,sizeof(m.cells));
    assert(get(&m,QUSB_SMMU_BASE+0x83058)==0x400U);
    assert(QcomUsbDomainInstall(&p,pt,m.table_pa,QUSB_DOMAIN_PT_BYTES,m.dma_pa,QUSB_DOMAIN_DMA_BYTES,&l)==QusbDomainOkay);
    assert(QcomUsbDomainRestore(&p,&l)==QusbDomainOkay);restored(&m);
  }
  }
  assert(QcomUsbDomainBuildTables(pt,0x1000000000000ULL,QUSB_DOMAIN_PT_BYTES,0x234560000ULL,32768,48)==QusbDomainBadArgument);
  assert(QcomUsbDomainBuildTables(pt,0x123450000ULL,QUSB_DOMAIN_PT_BYTES,0x123450000ULL,32768,44)==QusbDomainBadArgument);
  free(pt);puts("Private SMMUv2 translated domain: table walker, high PA, ownership and failure tests passed");
}
