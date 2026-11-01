"""Corruption, truncation and native wire authorization tests for the real sink."""
import gzip
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('decoder', ROOT/'scripts/decode-user-logs.py')
decoder = importlib.util.module_from_spec(spec); spec.loader.exec_module(decoder)


def frames(data, flags=2):
    return [f'QCOM_ULOG v=1 sid=0000000000000001 stream=1 seq={off//1024} total={len(data)} off={off} bytes={len(data[off:off+1024])} crc={zlib.crc32(data[off:off+1024]):08x} flags={flags} data={data[off:off+1024].hex()}\n'.encode()
            for off in range(0, len(data), 1024)]


class UserLogTransport(unittest.TestCase):
    def test_paged_source_requires_contiguous_pages_and_real_end(self):
        def page(number, first, count, last, total, payload):
            return (f'# qcom oslog pass 1 page={number} first_entry={first} scope=system\n'.encode()+payload+
                    f'# {count} entries\n# qcom_page pass=1 page={number} first={first} count={count} final={last} total={total}\n'.encode())
        one=page(1,1,1,0,1,b'0.000 70 SpringBoard ui:scene notice first\n')
        two=page(2,2,1,1,2,b'1.000 56 qcom-usb-log : error QCOM USERLOG CANARY pid=56 nonce=0123456789abcdef\n')
        def wire(text, stream):
            return b''.join(x.replace(b'stream=1 ',f'stream={stream} '.encode()) for x in frames(gzip.compress(text)))
        raw=wire(one,1)+wire(two,2);group=decoder.decode(raw)['groups'][0]
        self.assertTrue(group['complete']);self.assertEqual(group['entries'],[2])
        self.assertEqual(group['canary_nonces'],['0123456789abcdef'])
        self.assertEqual(group['processes'],{'SpringBoard':1,'qcom-usb-log':1})
        self.assertIn('first',group['text']);self.assertIn('QCOM USERLOG CANARY',group['text'])
        spec=importlib.util.spec_from_file_location('page_qualification',ROOT/'scripts/qualify-user-logs.py')
        qualification=importlib.util.module_from_spec(spec);spec.loader.exec_module(qualification)
        startup=b'collector_started version=2 upstream=88849ec system_scope=0 canary_nonce=0123456789abcdef'
        budget=b'collector_memory_budget stage=verified error=0 active_mb=128 active_attr=1 inactive_mb=128 inactive_attr=1'
        def status(text,stream):
            return b''.join(x.replace(b'stream=1 ',f'stream={stream} '.encode()) for x in frames(text,flags=1))
        pages=wire(one,3)+wire(two,4)
        self.assertTrue(qualification.completed_system_groups(decoder.decode(status(startup,1)+status(budget,2)+pages)))
        for prefix in [status(startup,1),status(startup.replace(b'0123456789abcdef',b'ffffffffffffffff'),1)+status(budget,2),
                       status(startup,1)+status(budget.replace(b'active_mb=128',b'active_mb=6'),2)]:
            self.assertFalse(qualification.completed_system_groups(decoder.decode(prefix+pages)))
        for bad in [wire(one,1),wire(two,1),wire(one,1)+wire(two.replace(b'first=2',b'first=3'),2),
                    wire(one,1)+wire(two.replace(b'total=2',b'total=3'),2),
                    raw+wire(two,3),wire(one,1)+wire(two.replace(b'final=1',b'final=0'),2),
                    wire(one,1)+wire(two.replace(b'# 1 entries',b'# 9 entries'),2),
                    wire(one,1)+wire(two.replace(b'page=2 first_entry',b'page=3 first_entry'),2),
                    wire(one,1)+wire(two.replace(b'1.000',b'# TRUNCATED\n1.000'),2)]:
            self.assertFalse(any(g['complete'] for g in decoder.decode(bad)['groups']))
        with tempfile.TemporaryDirectory() as directory:
            d=Path(directory);capture=d/'raw.log';output=d/'decoded'
            # A later incomplete pass must not replace the last complete group.
            capture.write_bytes(raw+wire(one.replace(b'pass 1',b'pass 2').replace(b'pass=1',b'pass=2'),3))
            run=subprocess.run(['python3',str(ROOT/'scripts/decode-user-logs.py'),str(capture),
                                '--output',str(output)],capture_output=True)
            self.assertEqual(run.returncode,0,run.stderr.decode())
            manifest=json.loads((output/'manifest.json').read_text())
            self.assertEqual(manifest['latest_complete_group'],['0000000000000001',1])
            latest=(output/'latest-oslog.txt').read_text()
            self.assertIn('first',latest);self.assertIn('QCOM USERLOG CANARY',latest)
            self.assertNotIn('pass 2',latest)

    def test_combined_coverage_requires_current_source_and_kernel_gates(self):
        spec = importlib.util.spec_from_file_location('combined', ROOT/'scripts/qualify-logs.py')
        combined = importlib.util.module_from_spec(spec); spec.loader.exec_module(combined)
        early = combined.load('qualify-early-log')
        records = [f'QCOM EARLY stage={s} counter_ticks={740000000+i*192} entry_ticks=740000000 freq_hz=19200000 delta_us={i*10}\n'.encode()
                   for i,s in enumerate(early.STAGES)]
        raw = b''.join(records[:12])+b'QCOM XNU USB CDC ACM v1\n'+b''.join(records[12:])
        raw += b'QCOM MSGBUF CANARY v=1\nwriter_mode=queued\n'
        def stream(data, number, flags=1):
            return b''.join(f.replace(b'stream=1 ',f'stream={number} '.encode()) for f in frames(data,flags))
        raw += stream(b'collector_started version=2 canary_nonce=0123456789abcdef',1)
        raw += stream(b'collector_memory_budget stage=verified error=0 active_mb=128 active_attr=1 inactive_mb=128 inactive_attr=1',2)
        text = (b'# qcom oslog pass 1 page=1 first_entry=1 scope=system\n'
                b'0.000 56 qcom-usb-log : error QCOM USERLOG CANARY pid=56 nonce=0123456789abcdef\n'
                b'0.001 70 SpringBoard : notice event\n'
                b'# 2 entries\n# qcom_page pass=1 page=1 first=1 count=2 final=1 total=2\n')
        raw += stream(gzip.compress(text),3,2)
        self.assertTrue(combined.qualify(raw)['verified'])
        spec = importlib.util.spec_from_file_location('capture_analysis', ROOT/'scripts/log_capture.py')
        analysis = importlib.util.module_from_spec(spec); spec.loader.exec_module(analysis)
        import hashlib
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            capture = folder/'xnu-usb.log'; capture.write_bytes(raw)
            metadata = dict(status='usb-eof', bytes=len(raw), synced_bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())
            (folder/'capture.json').write_text(json.dumps(metadata))
            self.assertTrue(analysis.verify_capture(folder)['verified'])
            self.assertEqual(capture.read_bytes(),raw)
            self.assertTrue((folder/'logs/latest-oslog.txt').is_file())
            with self.assertRaises(FileExistsError): analysis.verify_capture(folder)
            # Valid payload cannot qualify when the original synced identity differs.
            other = folder/'mismatch'; other.mkdir()
            (other/'xnu-usb.log').write_bytes(raw)
            (other/'capture.json').write_text(json.dumps(dict(metadata,synced_bytes=len(raw)-1)))
            self.assertFalse(analysis.verify_capture(other)['verified'])
            self.assertEqual(capture.read_bytes(),raw)

        for bad in [raw.replace(b'writer_mode=queued',b''),raw+b'QCOM USB LOSS bytes=1\n',
                    raw+b'QCOM MSGBUF CANARY v=1\n',raw.replace(records[1],b''),
                    raw.replace(b'canary_nonce=0123456789abcdef'.hex().encode(), b'canary_nonce=ffffffffffffffff'.hex().encode())]:
            self.assertFalse(combined.qualify(bad)['verified'])

    def test_collector_failed_attach_preserves_metadata_and_fails_verification(self):
        import sys
        from unittest.mock import patch
        spec = importlib.util.spec_from_file_location('collector', ROOT/'scripts/collect-xnu-usb.py')
        collector = importlib.util.module_from_spec(spec); spec.loader.exec_module(collector)
        spec = importlib.util.spec_from_file_location('log_capture', ROOT/'scripts/log_capture.py')
        analysis = importlib.util.module_from_spec(spec); spec.loader.exec_module(analysis)
        with tempfile.TemporaryDirectory() as directory:
            argv=['collect-xnu-usb.py','--output',directory,'--verify-logs']
            with patch.object(sys,'argv',argv), patch.object(collector,'find',side_effect=RuntimeError('fixture attach failure')), patch.dict(sys.modules,{'log_capture':analysis}):
                self.assertEqual(collector.main(),1)
            report=json.loads((Path(directory)/'capture.json').read_text())
            self.assertEqual(report['status'],'capture-failed')
            self.assertTrue(report['logging_verification_requested'])
            self.assertFalse(report['logging']['verified'])
            self.assertIn('fixture attach failure',report['error'])

    def test_native_memory_budget_is_self_only_and_requires_exact_readback(self):
        harness = r'''
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
static int mode, reads, writes;
static uid_t fake_euid(){return mode==1?501:0;}
static pid_t fake_pid(){return 56;}
static int fake_control(uint32_t cmd,int32_t pid,uint32_t flags,void *p,size_t size){
 assert(pid==56 && flags==0 && size==16);auto *v=(uint32_t *)p;
 if(cmd==8){
  ++reads;
  if(mode==3 || (mode==5 && reads==2)){errno=EACCES;return -1;}
  v[0]=reads==1?6:128;v[1]=reads==1?0:1;
  v[2]=reads==1?6:128;v[3]=1;
  if(mode==6 && reads==2)v[2]=6;
  if(mode==7 && reads==2)v[3]=0;
  return 0;
 }
 assert(cmd==7 && reads==1 && v[0]==128 && v[1]==1 && v[2]==128 && v[3]==1);
 ++writes;if(mode==4){errno=EACCES;return -1;}return 0;
}
static void *fake_dlsym(void *handle,const char *name){
 assert(handle==RTLD_DEFAULT && !strcmp(name,"memorystatus_control"));
 return mode==2?nullptr:reinterpret_cast<void *>(fake_control);
}
#define geteuid fake_euid
#define getpid fake_pid
#define dlsym fake_dlsym
#include "QcomLogMemory.h"
int main(){
 char text[320];
 for(mode=0;mode<8;++mode){
  reads=writes=0;bool ok=QcomUserLog::configureIos16MemoryBudget(text,sizeof text);
  assert(ok==(mode==0));
  if(mode==0){assert(reads==2 && writes==1);assert(strstr(text,"stage=verified error=0 active_mb=128 active_attr=1 inactive_mb=128 inactive_attr=1"));}
  if(mode==1 || mode==2)assert(!reads && !writes);
  if(mode==3)assert(reads==1 && !writes && strstr(text,"stage=before error=13"));
  if(mode==4)assert(reads==1 && writes==1 && strstr(text,"stage=set error=13"));
  if(mode==5)assert(reads==2 && writes==1 && strstr(text,"stage=readback error=13"));
  if(mode==6 || mode==7)assert(strstr(text,"stage=mismatch"));
 }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            d=Path(directory);(d/'test.cpp').write_text(harness)
            result=subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',
                                   '-fsanitize=address,undefined','-I'+str(ROOT/'userland'),
                                   str(d/'test.cpp'),'-o',str(d/'test')],capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr.decode())
            subprocess.run([str(d/'test')],check=True,timeout=10)

    def test_coverage_rejects_collector_only_or_loader_error_snapshots(self):
        spec = importlib.util.spec_from_file_location('qualification', ROOT/'scripts/qualify-user-logs.py')
        qualification = importlib.util.module_from_spec(spec); spec.loader.exec_module(qualification)
        canary = b'0.000 56 qcom-usb-log : error QCOM USERLOG CANARY pid=56 nonce=0123456789abcdef\n'
        expected = {'0123456789abcdef'}
        external = b'0.100 70 SpringBoard com.apple.ui:scene notice scene changed\n'
        def snapshot(text):
            return decoder.decode(b''.join(frames(gzip.compress(text))))['streams'][0]
        self.assertFalse(qualification.usable_system_snapshot(snapshot(canary+b'# 1 entries\n'),expected))
        system = snapshot(canary+external+b'# 2 entries\n')
        self.assertTrue(qualification.usable_system_snapshot(system,expected))
        self.assertFalse(qualification.usable_system_snapshot(system,{'ffffffffffffffff'}))
        self.assertFalse(qualification.usable_system_snapshot(system,set()))
        failed = snapshot(b'# framework load failed: missing dependency\n'+canary+external+b'# 2 entries\n')
        self.assertIn('# framework load failed:', failed['coverage_errors'])
        self.assertFalse(qualification.usable_system_snapshot(failed,expected))

    def test_exporter_reports_loader_failure_and_retries_next_pass(self):
        exporter = (ROOT/'userland/QcomOsLogExport.mm').read_text().replace('#include "QcomOsLogPages.inc"', (ROOT/'userland/QcomOsLogPages.inc').read_text())
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            exporter = exporter.replace('"/private/var/tmp/qcom-oslog"', json.dumps(str(d)))
            (d/'QcomUserLogTransport.h').write_text('''#include <string>
#include <vector>
#include <dlfcn.h>
namespace QcomUserLog {
static std::vector<std::string> statuses;
static bool status(const char *s){statuses.emplace_back(s);return true;}
static bool snapshot(const char *,unsigned){return true;}
}
''')
            (d/'QcomIos16LogStore.h').write_text('namespace QcomUserLog {static id openIos16SystemStore(Class,NSError **){return nil;}}')
            # Compile the actual staged Objective-C++ exporter. Only the loader
            # and class lookup are replaced; gzip, rename and retry logic run.
            harness = r'''
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <assert.h>
#include <zlib.h>
#include <string>
static bool loadSucceeds;
static unsigned opens;
static void *fake_dlopen(const char *,int){++opens;return loadSucceeds?(void *)1:nullptr;}
static const char *fake_dlerror(){return "fixture dependency missing";}
static Class fake_class(NSString *){return Nil;}
#define dlopen fake_dlopen
#define dlerror fake_dlerror
#define NSClassFromString fake_class
'''+exporter+r'''
static std::string contents(){
 gzFile file=gzopen((std::string(kDirectory)+"/oslog.txt.gz").c_str(),"rb");assert(file);
 char text[4096];int size=gzread(file,text,sizeof text);assert(size>0);assert(gzclose(file)==Z_OK);
 return std::string(text,size);
}
int main(){@autoreleasepool {
 writePass(1);assert(opens==1);
 assert(contents().find("# framework load failed: fixture dependency missing")!=std::string::npos);
 assert(QcomUserLog::statuses.size()==2&&QcomUserLog::statuses[0]=="oslog_framework_load_failed");
 loadSucceeds=true;writePass(2);assert(opens==2);
 assert(contents().find("# framework load failed:")==std::string::npos);
 assert(contents().find("# no store: OSLogStore missing")!=std::string::npos);
 writePass(3);assert(opens==2);
}}
'''
            (d/'test.mm').write_text(harness)
            result = subprocess.run(['clang++','-std=c++17','-fobjc-arc','-framework','Foundation','-lz',
                                     '-I'+str(d),'-I'+str(ROOT/'userland'),str(d/'test.mm'),'-o',str(d/'test')],capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr.decode())
            subprocess.run([str(d/'test')],check=True,timeout=15)

    def test_gzip_integrity_gaps_duplicates_and_crc(self):
        # An incompressible deterministic payload needs multiple wire records.
        import random
        text = random.Random(123).randbytes(7000)
        wire = frames(gzip.compress(text))
        result = decoder.decode(b'kernel prefix\n'+b''.join(wire))
        self.assertTrue(result['streams'][0]['complete'])
        self.assertFalse(result['frame_errors'])
        gap=decoder.decode(b''.join(wire)+b''.join(x.replace(b'stream=1 ',b'stream=3 ') for x in wire))
        self.assertEqual(gap['stream_gaps'],[{'session':'0000000000000001','first_missing':2,'last_missing':2}])
        self.assertEqual(decoder.decode(b''.join(wire[:1]+wire[2:]))['streams'][0]['complete'], False)
        self.assertEqual(decoder.decode(b''.join(wire[:-1]))['streams'][0]['complete'], False)
        self.assertTrue(decoder.decode(b''.join(wire+[wire[-1]]))['streams'][0]['complete'])
        bad = wire[0].replace(b' crc=', b' crc=ffffffff', 1)
        self.assertTrue(decoder.decode(bad)['frame_errors'])
        self.assertFalse(decoder.decode(b''.join(frames(b'bad gzip')))['streams'][0]['complete'])
        restricted = decoder.decode(b''.join(frames(gzip.compress(b'# no store: not authorized\n'))))
        self.assertEqual(restricted['streams'][0]['coverage_errors'], ['# no store:'])

    def test_actual_exporter_pages_without_losing_entries_and_bounds_failures(self):
        exporter = (ROOT/'userland/QcomOsLogExport.mm').read_text().replace('#include "QcomOsLogPages.inc"', (ROOT/'userland/QcomOsLogPages.inc').read_text())
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            exporter = exporter.replace('"/private/var/tmp/qcom-oslog"', json.dumps(str(d)))
            (d/'QcomUserLogTransport.h').write_text('''#include <string>
#include <vector>
#include <dlfcn.h>
#include <zlib.h>
#include <assert.h>
namespace QcomUserLog {
static std::vector<std::string> statuses;
static bool status(const char *s){statuses.emplace_back(s);return true;}
static std::vector<std::string> snapshots;
static bool failSnapshot;
static bool snapshot(const char *path,unsigned){
 gzFile file=gzopen(path,"rb");assert(file);std::string text;char buffer[4096];int n;
 while((n=gzread(file,buffer,sizeof buffer))>0)text.append(buffer,n);
 assert(n==0 && gzclose(file)==Z_OK);snapshots.emplace_back(text);return !failSnapshot;
}
}
''')
            (d/'QcomIos16LogStore.h').write_text('namespace QcomUserLog {static id openIos16SystemStore(Class,NSError **){return nil;}}')
            # Model a native framework's autoreleased fetch temporaries in a
            # separate non-ARC unit, not just strong entry variables in ARC.
            (d/'pool.mm').write_text(r'''
#import <Foundation/Foundation.h>
#include <assert.h>
static NSUInteger live, peak;
@interface QcomFetchTemporary : NSObject
@end
@implementation QcomFetchTemporary
- (id)init {if((self=[super init])){++live;if(live>peak)peak=live;}return self;}
- (void)dealloc {assert(live);--live;[super dealloc];}
@end
extern "C" void fetchTemporary(){[[[QcomFetchTemporary alloc] init] autorelease];}
extern "C" NSUInteger peakTemporary(){return peak;}
extern "C" NSUInteger liveTemporary(){return live;}
''')
            harness = r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#include <string>
#include <zlib.h>
extern "C" void fetchTemporary();
extern "C" NSUInteger peakTemporary();
extern "C" NSUInteger liveTemporary();
static NSUInteger count, payloadBytes;
@interface QcomLogFixtureEntry : NSObject
@end
@implementation QcomLogFixtureEntry
- (NSDate *)date {return [NSDate dateWithTimeIntervalSince1970:1];}
- (NSString *)composedMessage {return payloadBytes?[@"x" stringByPaddingToLength:payloadBytes withString:@"x" startingAtIndex:0]:@"fixture external entry";}
- (NSString *)process {return @"SpringBoard";}
- (NSString *)subsystem {return @"fixture";}
- (NSString *)category {return @"test";}
- (int)processIdentifier {return 70;}
- (NSInteger)level {return 3;}
@end
@interface QcomLogFixtureEnumerator : NSEnumerator {
    NSUInteger cursor;
}
@end
@implementation QcomLogFixtureEnumerator
- (id)nextObject {
 if(cursor==count)return nil;
 ++cursor;fetchTemporary();return [QcomLogFixtureEntry new];
}
@end
@interface QcomLogFixtureStore : NSObject
@end
@implementation QcomLogFixtureStore
+ (id)storeWithScope:(NSInteger)scope error:(NSError **)error {assert(scope==0);return [self new];}
- (id)positionWithTimeIntervalSinceLatestBoot:(double)at {assert(at==0);return @0;}
- (id)entriesEnumeratorWithOptions:(NSUInteger)options position:(id)position predicate:(id)predicate error:(NSError **)error {
 assert(options==0 && [position isEqual:@0] && !predicate);
 return [QcomLogFixtureEnumerator new];
}
@end
static Class fake_class(NSString *){return [QcomLogFixtureStore class];}
static void *fake_dlopen(const char *,int){return (void *)1;}
#define NSClassFromString fake_class
#define dlopen fake_dlopen
'''+exporter+r'''
static std::string contents(){
 gzFile file=gzopen((std::string(kDirectory)+"/oslog.txt.gz").c_str(),"rb");assert(file);
 std::string result;char buffer[4096];int n;
 while((n=gzread(file,buffer,sizeof buffer))>0)result.append(buffer,n);
 assert(n==0 && gzclose(file)==Z_OK);return result;
}
static bool hasStatus(const char *value){
 for(const auto &s:QcomUserLog::statuses)if(s==value)return true;return false;
}
int main(){@autoreleasepool {
 count=4100;writePass(1);auto text=contents();
#ifdef EXPECT_OUTER_POOL
 assert(peakTemporary()>=4100 && liveTemporary()==0);return 0;
#else
 assert(peakTemporary()<=1 && liveTemporary()==0);
#endif
 assert(text.find("# 4100 entries\n")!=std::string::npos);
 assert(text.find("70 SpringBoard fixture:test notice fixture external entry")!=std::string::npos);
 assert(text.find("TRUNCATED")==std::string::npos);
 assert(hasStatus("oslog_enumeration_begin pass=1") && hasStatus("oslog_enumerator ready=1"));
 assert(hasStatus("oslog_enumeration_progress pass=1 entries=1"));
 assert(hasStatus("oslog_enumeration_progress pass=1 entries=4096"));
 assert(hasStatus("oslog_enumeration_end pass=1 entries=4100"));
 using QcomUserLog::snapshots;
 snapshots.clear();count=60001;writePass(2);assert(snapshots.size()==2);
 assert(snapshots[0].find("page=1 first=1 count=60000 final=0 total=60000")!=std::string::npos);
 assert(snapshots[1].find("page=2 first=60001 count=1 final=1 total=60001")!=std::string::npos);
 assert(snapshots[0].find("TRUNCATED")==std::string::npos && snapshots[1].find("TRUNCATED")==std::string::npos);
 assert(peakTemporary()<=1 && liveTemporary()==0);
 assert(hasStatus("oslog_enumeration_end pass=2 entries=60001"));
 snapshots.clear();count=1000;payloadBytes=20000;writePass(3);assert(snapshots.size()==3);
 for(const auto &page:snapshots)assert(page.size()<=8388608 && page.find("TRUNCATED")==std::string::npos);
 assert(snapshots.back().find("final=1 total=1000")!=std::string::npos);
 snapshots.clear();count=1;payloadBytes=70000;writePass(4);assert(snapshots.size()==1);
 assert(snapshots[0].find("# TRUNCATED oversized_or_invalid_entry")!=std::string::npos);
 assert(snapshots[0].find("final=0 total=0")!=std::string::npos);
 snapshots.clear();payloadBytes=0;count=480001;writePass(5);assert(snapshots.size()==8);
 assert(snapshots.back().find("# TRUNCATED page_limit")!=std::string::npos);
 assert(snapshots.back().find("final=0 total=480000")!=std::string::npos);
 snapshots.clear();QcomUserLog::failSnapshot=true;count=60001;writePass(6);assert(snapshots.size()==1);
 assert(snapshots[0].find("final=0 total=60000")!=std::string::npos);
}}
'''
            (d/'test.mm').write_text(harness)
            pool = subprocess.run(['clang++','-fsanitize=address,undefined','-c',str(d/'pool.mm'),
                                   '-o',str(d/'pool.o')],capture_output=True)
            self.assertEqual(pool.returncode,0,pool.stderr.decode())
            result = subprocess.run(['clang++','-std=c++17','-fobjc-arc','-fsanitize=address,undefined',
                                     '-framework','Foundation','-lz','-I'+str(d),'-I'+str(ROOT/'userland'),str(d/'test.mm'),
                                     str(d/'pool.o'),'-o',str(d/'test')],capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr.decode())
            subprocess.run([str(d/'test')],check=True,timeout=30)
            # Negative control: the previous real loop accumulates all fetch
            # temporaries until writePass's outer pool drains.
            legacy=harness.replace('for (;;) {\n                    @autoreleasepool {\n                        id entry = ((id (*)(id, SEL))objc_msgSend)(entries, @selector(nextObject));\n                        if (!entry) break;',
                                   'for (id entry in entries) {\n                    @autoreleasepool {')
            self.assertNotEqual(legacy,harness)
            (d/'legacy.mm').write_text(legacy)
            result=subprocess.run(['clang++','-std=c++17','-fobjc-arc','-fsanitize=address,undefined',
                                   '-DEXPECT_OUTER_POOL=1','-framework','Foundation','-lz','-I'+str(d),
                                   '-I'+str(ROOT/'userland'),str(d/'legacy.mm'),str(d/'pool.o'),'-o',str(d/'legacy')],capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr.decode())
            subprocess.run([str(d/'legacy')],check=True,timeout=10)


if __name__ == "__main__": unittest.main()
