"""Exercise the actual sysctl sink, atomic ring and userland sender."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from source_helpers import function
ROOT = Path(__file__).resolve().parents[1]

class NativeLogging(unittest.TestCase):
    def compile_run(self, text, cpp=False):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory); src=d/('test.cpp' if cpp else 'test.c'); src.write_text(text)
            p = subprocess.run(['clang++' if cpp else 'clang', '-std=c++17' if cpp else '-std=c11',
                '-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined',
                '-I'+str(ROOT/'include'),'-I'+str(ROOT/'kernel'),'-I'+str(ROOT/'userland'),
                str(src),'-o',str(d/'test')],capture_output=True)
            self.assertEqual(p.returncode,0,p.stderr.decode())
            subprocess.run([str(d/'test')], check=True, timeout=15)

    def test_sysctl_root_copy_bounds_crc_and_atomic_retry(self):
        source=(ROOT/'kernel/QcomUsbUserLog.c').read_text().replace('qcom_usb_userlog_sysctl SYSCTL_HANDLER_ARGS', 'qcom_usb_userlog_sysctl(void *oidp,void *arg1,int arg2,struct request *req)')
        actual='int\n'+function(source,'qcom_usb_userlog_submit')
        # Keep the actual syscall wrapper too, including its copyin boundary.
        actual+='static int\n'+function(source,'qcom_usb_userlog_sysctl')
        self.compile_run(r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "QcomUserLogWire.h"
#include "QcomUsbBuffer.h"
static int root=1, copy_error;
static void *current_proc(void){return 0;}
static int proc_suser(void *p){(void)p;return root?0:1;}
static unsigned char data[4096];static struct soc_usb_buffer ring={.data=data,.capacity=sizeof data};
static size_t qcom_usb_debug_try_record(const void *p,size_t n){return soc_usb_buffer_try_record(&ring,p,n);}
struct request { void *oldptr,*newptr;size_t newlen; };
#define SYSCTL_HANDLER_ARGS (void *oidp,void *arg1,int arg2,struct request *req)
static int copyin(struct request *req,void *out,size_t n){if(copy_error)return copy_error;memcpy(out,req->newptr,n);return 0;}
#define SYSCTL_IN(req,out,n) copyin(req,out,n)
'''+actual+r'''
int main(void){
 struct QcomUserLogWire w={.magic=SOC_ULOG_MAGIC,.version=1,.stream=1,.flags=SOC_ULOG_STATUS,.total=4,.bytes=4};
 memcpy(w.data,"test",4);w.crc=qcom_ulog_crc(w.data,4);
 size_t n=offsetof(struct QcomUserLogWire,data)+4;
 struct request req={.newptr=&w,.newlen=n};
 root=0;assert(qcom_usb_userlog_sysctl(0,0,0,&req)==EPERM&&!ring.used);root=1;
 req.newlen=sizeof w+1;assert(qcom_usb_userlog_sysctl(0,0,0,&req)==EINVAL);req.newlen=n;
 req.oldptr=&w;assert(qcom_usb_userlog_sysctl(0,0,0,&req)==EINVAL);req.oldptr=0;
 copy_error=EFAULT;assert(qcom_usb_userlog_sysctl(0,0,0,&req)==EFAULT);copy_error=0;
 w.crc++;assert(qcom_usb_userlog_sysctl(0,0,0,&req)==EINVAL);w.crc--;
 w.sequence=2;assert(qcom_usb_userlog_submit(&w,n)==EINVAL);w.sequence=0;
 w.reserved=1;assert(qcom_usb_userlog_submit(&w,n)==EINVAL);w.reserved=0;
 w.total=SOC_ULOG_MAX+1;assert(qcom_usb_userlog_submit(&w,n)==EINVAL);w.total=4;
 ring.busy=1;assert(qcom_usb_userlog_submit(&w,n)==EAGAIN&&!ring.used);ring.busy=0;
 ring.used=ring.capacity-1;assert(qcom_usb_userlog_submit(&w,n)==EAGAIN);ring.used=0;
 assert(qcom_usb_userlog_sysctl(0,0,0,&req)==0);
 assert(strstr((char*)data,"data=74657374\n"));assert(!ring.overflow&&!ring.contended);
}
''')

    def test_sysctl_sender_exact_retry_and_permanent_error(self):
        self.compile_run(r'''
#include <sys/sysctl.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include "QcomUserLogWire.h"
static int calls, waits, mode;static QcomUserLogWire first;
static int fake_usleep(unsigned n){assert(n==100000||n==125000);++waits;return 0;}
static int fake_sysctl(const char *key,void *out,size_t *outlen,void *in,size_t n){
 assert(!strcmp(key,"debug.qcom_usb_userlog")&&!out&&!outlen);
 auto *w=(QcomUserLogWire*)in;assert(qcom_ulog_valid(w,n));
 if(!calls) first=*w;else assert(!memcmp(&first,w,sizeof first));
 ++calls;if(mode==1){errno=EIO;return -1;}
 if(mode==2||calls<3){errno=EAGAIN;return -1;}return 0;
}
#define sysctlbyname fake_sysctl
#define usleep fake_usleep
#include "QcomUserLogTransport.h"
int main(){using namespace QcomUserLog;QcomUserLog::session=123;
 QcomUserLogWire w{};w.stream=1;w.total=w.bytes=4;w.flags=SOC_ULOG_STATUS;memcpy(w.data,"test",4);
 assert(send(w));assert(calls==3&&waits==3&&retries==2&&!failed);
 calls=waits=0;mode=1;assert(!send(w));assert(calls==1&&!waits&&failed==1);
 calls=waits=0;mode=2;assert(!send(w));assert(calls==100&&waits==100&&failed==2);
}
''',True)

    def test_actual_worker_write_and_shutdown_quiet_window(self):
        source=(ROOT/'kernel/QcomXnuUsbDebug.c').read_text().replace('size_t qcom_usb_debug_write(', 'size_t\nqcom_usb_debug_write(').replace('int qcom_usb_debug_wait_drained(', 'int\nqcom_usb_debug_wait_drained(')
        write='size_t\n'+function(source,'qcom_usb_debug_write')
        drain='int\n'+function(source,'qcom_usb_debug_wait_drained')
        self.compile_run(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define QCOM_USB_DEBUG_DISABLED 1
#define QCOM_USB_DEBUG_STOPPED 2
#define QCOM_USB_DEBUG_ACTIVE 3
static struct { struct {int phase;uint64_t counter_frequency,buffer_pending,transport_pending;} status;void *core;} native;
static int usb_worker_ready,usb_diag_enabled,polls,mode;static uint64_t now;
static size_t backlog_write(const void *p,size_t n){(void)p;return n;}
static int acquire(uint64_t *f){*f=0;return 1;}
static void release(uint64_t f){(void)f;}
static int poll_owned(void){polls++;return mode!=2;}
static int dwc3_ready(void *p){(void)p;return 0;}
static size_t dwc3_queued(void *p){(void)p;return 0;}
static int usb_log_buffer;
static size_t soc_usb_buffer_pending(void *p){(void)p;return 0;}
static uint64_t counter(void){return now;}
static void delay_us(int c,unsigned u){(void)c;now+=u;}
static void buffer_status_owned(void){native.status.buffer_pending=(mode==1&&now==100000)||mode==3;}
'''+write+drain+r'''
int main(void){native.status.phase=3;native.status.counter_frequency=1000000;
 usb_worker_ready=1;assert(qcom_usb_debug_write("x",1)==1&&!polls);
 usb_worker_ready=0;assert(qcom_usb_debug_write("x",1)==1&&polls==1);
 assert(qcom_usb_debug_wait_drained(2000)&&now==250000);
 now=0;mode=1;assert(qcom_usb_debug_wait_drained(2000)&&now==351000);
 now=0;mode=2;assert(!qcom_usb_debug_wait_drained(2000));
 now=0;mode=3;assert(!qcom_usb_debug_wait_drained(20)&&now==20000);
}
''')

if __name__=='__main__': unittest.main()
