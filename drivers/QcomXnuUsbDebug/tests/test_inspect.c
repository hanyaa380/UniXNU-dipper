/* SPDX-License-Identifier: MIT */
#include "soc/QcomUsbQualify.h"
#include <assert.h>
static unsigned seen;
static int rd(void*c,QUSB_U64 a,QUSB_U32*v){
 (void)c; const QUSB_U32 clocks[]=QUD_GCC_CBCR_OFFSETS; *v=0;
 for(unsigned i=0;i<5;i++)if(a==QUSB_GCC_BASE+clocks[i]){seen|=1u<<i;return 1;}
 if(a==QUSB_GCC_BASE+QUD_GCC_CFG_GDSCR_OFFSET){*v=1u<<16;seen|=32;return 1;}
 if(a==QUSB_GCC_BASE+QUD_GCC_BCR_OFFSET||a==QUSB_GCC_BASE+QUD_GCC_GDSCR_OFFSET)return 1;
 if(a==QUSB_DWC3_BASE+0xc120){*v=0x5533330a;seen|=64;return 1;}
 if(a>=QUSB_DWC3_BASE+0xc000&&a<QUSB_DWC3_BASE+0xc800)return 1;
 assert(0);return 0;
}
int main(void){QUSB_INHERITED s;assert(QcomUsbInspectInherited(rd,0,&s)==QusbQualified);assert(seen==127);}
