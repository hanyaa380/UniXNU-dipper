"""Execute the actual early formatter and patched kprintf/startup transition."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile
import unittest
import os

from source_helpers import function

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ['QUD_TEST_XNU'])


class EarlyLog(unittest.TestCase):
    def test_coverage_rejects_old_missing_or_inconsistent_epoch(self):
        spec = importlib.util.spec_from_file_location('early', ROOT/'scripts/qualify-early-log.py')
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        lines = [f'QCOM EARLY stage={s} counter_ticks={740000000+i*192} entry_ticks=740000000 freq_hz=19200000 delta_us={i*10}\n'.encode()
                 for i,s in enumerate(helper.STAGES)]
        raw = b''.join(lines[:12])+b'QCOM XNU USB CDC ACM v1\n'+b''.join(lines[12:])
        result = helper.qualify(raw)
        self.assertTrue(result['passed'])
        self.assertEqual(result['entry_to_last_stage_us'], 140)
        self.assertFalse(helper.qualify(b'[39.000] old boot\n')['passed'])
        self.assertFalse(helper.qualify(raw.replace(lines[2], b''))['passed'])
        self.assertFalse(helper.qualify(raw.replace(b'freq_hz=19200000', b'freq_hz=19200001', 1))['passed'])
        self.assertFalse(helper.qualify(raw.replace(b'delta_us=0', b'delta_us=invalid', 1))['passed'])
        self.assertFalse(helper.qualify(raw+b'QCOM USB LOSS bytes=1\n')['passed'])

    def test_early_records_and_actual_kprintf_transition(self):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            for name in ('pexpert/arm/pe_kprintf.c', 'osfmk/arm/arm_init.c', 'osfmk/arm64/start.s'):
                p = d/name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(subprocess.check_output(['git','-C',str(SOURCE),'show','HEAD:'+name]))
            native = (ROOT/'patches/darwin22/0001-qcom-usb-debug-console.patch').read_text().split('--- a/osfmk/kern/debug.c')[0]
            subprocess.run(['patch', '-p1', '--batch'], input=native.encode(), cwd=d, check=True, capture_output=True)
            subprocess.run(['patch', '-p1', '--batch', '--input', str(ROOT/'patches/darwin22/0004-qcom-usb-debug-early-log.patch')],
                           cwd=d, check=True, capture_output=True)
            text = (d/'pexpert/arm/pe_kprintf.c').read_text()
            actual = 'void\n'+function(text, 'qcom_early_log_stage')
            actual = actual.replace('__asm__ volatile("isb\\nmrs %0, cntvct_el0" : "=r"(now) :: "memory");', 'now = fake_counter;')
            actual = actual.replace('__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));', 'frequency = 19200000;')
            self.assertNotIn('__asm__', actual)
            actual += 'void\n'+function(text, 'kprintf')
            actual += 'static void\n'+function(text, 'PE_init_kprintf')
            harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "QcomEarlyLog.h"
#define XNU_QCOM_USB_DEBUG 1
#define FALSE 0
#define LCK_GRP_NULL 0
#define MP_DEBUG_KPRINTF(...) ((void)0)
#define OS_LOG_DEFAULT 0
#define OS_LOG_TYPE_DEFAULT 0
uint64_t qcom_early_entry_ticks = 740000000;
static uint64_t fake_counter = 740000192;
static bool qcom_early_kprintf = true, qcom_usb_debug_kprintf_requested = true;
static bool disable_serial_output, disable_kprintf_output = true;
static int kprintf_lock, cpu_last_locked;
static int locks, interrupts, cpus, native_logs, hardware;
static char output[20000]; static size_t length;
static void (*PE_kputc)(char);
static void qcom_usb_debug_enqueue_putc(char c) { assert(length+1<sizeof output); output[length++]=c; output[length]=0; }
#define QCOM_USB_DEBUG_KPRINTF_PUTC qcom_usb_tee
static void qcom_usb_debug_putc(char c) { qcom_usb_debug_enqueue_putc(c); }
static void qcom_usb_tee(char c) { qcom_usb_debug_enqueue_putc(c); }
static void _doprnt_log(const char *fmt, va_list *args, void(*putc)(char), int radix) {
 (void)radix; char line[200]; int n=vsnprintf(line,sizeof line,fmt,*args); assert(n>=0&&n<(int)sizeof line);
 for(int i=0;i<n;i++)putc(line[i]);
}
typedef int boolean_t;
static int ml_set_interrupts_enabled(int state) { interrupts++; return state; }
static int simple_lock_try(int *lock,int group) { (void)lock;(void)group; locks++;return 1; }
static void simple_unlock(int *lock) { (void)lock; }
static int cpu_number(void) { cpus++;return 0; }
static void os_log_with_args(int a,int b,const char *fmt,va_list args,void *caller) {
 (void)a;(void)b;(void)fmt;(void)args;(void)caller;native_logs++;
}
static int serial_init(void) { return 0; }
static void serial_putc_crlf(char c) { (void)c;assert(0); }
static void console_write_unbuffered(char c) { (void)c;assert(0); }
static int qcom_usb_debug_init(void) { hardware++; return 1; }
@ACTUAL@
int main(void) {
 qcom_early_log_stage("arm_init_rebased");
 assert(strstr(output,"stage=start_first_cpu counter_ticks=740000000 entry_ticks=740000000 freq_hz=19200000 delta_us=0\n"));
 assert(strstr(output,"stage=arm_init_rebased counter_ticks=740000192 entry_ticks=740000000 freq_hz=19200000 delta_us=10\n"));
 assert(!locks&&!interrupts&&!cpus&&!hardware);
 length=0;output[0]=0;
 kprintf("early %d\n",42);
 assert(!strcmp(output,"early 42\n"));assert(native_logs==1&&!locks&&!cpus&&!interrupts&&!hardware);
 PE_init_kprintf();assert(hardware==1&&!qcom_early_kprintf&&!disable_kprintf_output);
 assert(strstr(output,"stage=usb_init_begin")&&strstr(output,"stage=usb_init_end"));
 length=0;output[0]=0;kprintf("late %d\n",43);
 assert(!strcmp(output,"late 43\n"));assert(locks==1&&cpus==1&&interrupts==2&&native_logs==2);
 length=0;output[0]=0;qcom_early_emit("bad",1,2,19200000,qcom_usb_debug_enqueue_putc);
 assert(strstr(output,"delta_us=invalid\n"));
 length=0;output[0]=0;qcom_early_emit("bad",1,0,0,qcom_usb_debug_enqueue_putc);
 assert(strstr(output,"delta_us=invalid\n"));
 length=0;output[0]=0;qcom_early_emit("bad",UINT64_MAX,UINT64_MAX,19200000,qcom_usb_debug_enqueue_putc);
 assert(strstr(output,"entry_ticks=18446744073709551615")&&strstr(output,"delta_us=invalid\n"));
 length=0;output[0]=0;qcom_early_emit("wrap",UINT64_MAX,0,1,qcom_usb_debug_enqueue_putc);
 assert(strstr(output,"delta_us=invalid\n"));
 char long_stage[100];memset(long_stage,'x',sizeof long_stage);long_stage[99]=0;
 length=0;output[0]=0;qcom_early_emit(long_stage,1,0,1000000,qcom_usb_debug_enqueue_putc);
 assert(strstr(output,"[truncated]")&&strstr(output,"delta_us=1\n"));
 return 0;
}
'''
            c = d/'fixture.c'
            c.write_text(harness.replace('@ACTUAL@', actual))
            result = subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I'+str(ROOT/'include'), str(c), '-o', str(d/'fixture')], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            subprocess.run([str(d/'fixture')], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
