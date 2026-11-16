"""Execute the actual Darwin22 putchar/vprintf_log_locked, including constty routes."""
from pathlib import Path
import subprocess
import tempfile
import re
import unittest
import os
from source_helpers import function

ROOT = Path(__file__).resolve().parents[1]


class Msgbuf(unittest.TestCase):
    def test_actual_patched_msgbuf_has_one_usb_copy_without_mmio(self):
        source = Path(os.environ['QUD_TEST_XNU'])
        original = subprocess.check_output(['git','-C',str(source),'show','HEAD:bsd/kern/subr_prf.c'], text=True)
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory); (d/'bsd/kern').mkdir(parents=True)
            p = d/'bsd/kern/subr_prf.c'; p.write_text(original)
            subprocess.run(['patch','-p1','--batch','--input',str(ROOT/'patches/darwin22/0003-qcom-usb-debug-msgbuf.patch')], cwd=d, check=True, capture_output=True)
            text = p.read_text()
            actual = 'void\n'+function(text, 'putchar')
            actual += 'bool\n'+function(text, 'vprintf_log_locked')
            actual = re.sub(r'\bputchar\b', 'apple_putchar', actual)
            harness = r'''
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#define XNU_QCOM_USB_DEBUG 1
#define TOCONS 1
#define TOTTY 2
#define TOLOG 4
#define TOLOGLOCKED 8
#define TOSTR 16
#define TRUE 1
struct putchar_args{int flags;void *tty;bool last_char_was_cr;};
static void *constty,*msgbufp;static char *debugger_panic_str;
static int enable_dklog_serial_output,tty_result,usb_count,msg_count;
static char usb[1000];
static int tputchar(int c,void *tty){(void)c;(void)tty;return tty_result;}
static void qcom_usb_debug_enqueue_putc(char c){usb[usb_count++]=c;}
static void console_write_char(int c){qcom_usb_debug_enqueue_putc((char)c);}
static void log_putc(int c){(void)c;msg_count++;}
static void log_putc_locked(void *b,int c){(void)b;log_putc(c);}
static void __doprnt(const char *fmt,va_list ap,void(*fn)(int,void*),void *arg,int radix,int mode){
 (void)radix;(void)mode;char text[500];vsnprintf(text,sizeof text,fmt,ap);for(char *c=text;*c;c++)fn(*c,arg);
}
@ACTUAL@
static bool emit(const char *fmt,...){va_list ap;va_start(ap,fmt);bool r=vprintf_log_locked(fmt,ap,false);va_end(ap);return r;}
int main(){
 assert(emit("canary %d\n",123));assert(usb_count==11&&msg_count==11&&!memcmp(usb,"canary 123\n",11));
 usb_count=msg_count=0;struct putchar_args args={TOLOG|TOCONS,0,false};apple_putchar('A',&args);assert(usb_count==1&&msg_count==1);
 usb_count=msg_count=0;constty=(void*)1;args=(struct putchar_args){TOLOG|TOCONS,0,false};apple_putchar('B',&args);assert(usb_count==1&&msg_count==1);
 usb_count=msg_count=0;tty_result=-1;args=(struct putchar_args){TOLOG|TOCONS,0,false};apple_putchar('C',&args);assert(usb_count==1&&msg_count==1);
 usb_count=msg_count=0;args=(struct putchar_args){TOLOGLOCKED,0,false};apple_putchar(0,&args);apple_putchar('\r',&args);apple_putchar(0177,&args);assert(!usb_count&&!msg_count);
 return 0;
}
'''
            c = d/'test.c'; c.write_text(harness.replace('@ACTUAL@', actual))
            result = subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(d/'test')], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            subprocess.run([str(d/'test')], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
