#!/usr/bin/env python3
"""fajita's CPU-rendered display archive: NativeLegacyDisplay bound to the unchanged original IOMobileFramebufferLegacy, with the
HID input device and the keystore notification service, in the archive the kernel link expects for CONFIG_XNU_NATIVE_LEGACY_DISPLAY
(build/native-legacy-display/libNativeLegacyDisplay.late.a and its build-report.json, scripts/build-xnu-4k-baseline.sh).

A port of UniXNU-sheng scripts/build-native-legacy-display.py (d92e799) that also runs on Linux:
- the tools: "xcrun clang" and "xcrun ar" are the shims of scripts/unixnu/host on Linux; llvm-objcopy comes from PATH, or from
  Homebrew's llvm on macOS. scripts/build-native-hid-input.py of UniXNU asks "brew --prefix llvm" for its llvm-objcopy: on a host
  without Homebrew it gets a one-line brew stand-in (build/native-legacy-display/host-bin) that names the prefix of PATH's llvm-objcopy.
- the reference candidate: the IOMobileGraphicsFamily and IOHIDFamily instruction checks need a prelinked kernel of the 21H461
  kernelcache. FAJITA_DISPLAY_CANDIDATE (default FAJITA_STORAGE_CANDIDATE, as scripts/build-fajita-storage.py) names a fajita
  kernel.macho with its kernel.json beside it; the audits accept it because it carries the identical original family executables
  (checked by hash in audit-mobile-graphics-legacy.py and build-native-hid-input.py).
- the runtime observation: audit-mobile-graphics-legacy.py carries the hash of the pruned QEMU observation of the original class
  (identity, parent, object size 0x18a0) over from its previous output. A fresh tree has none, so the recorded hash is seeded.

Contents (sheng's list without its board parts):
- drivers/NativeMobileDisplay (fajita's copy in this repository, with fajita-display-logical; see its README) NativeLegacyDisplay.cpp
  and NativeGOPPresenter.cpp (LegacyDisplayProbe registers both metaclasses;
  the presenter only acts with sheng-fb-gpu=1, which fajita does not set);
- drivers/NativeHidInput/NativeHidInput.cpp with its original-IOHIDDevice bindings (scripts/build-native-hid-input.py);
- drivers/NativeKeyStoreEvents/NativeKeyStoreEvents.cpp (the owned AppleKeyStore of sheng-aks=1);
- kits/fajita LegacyDisplayProbe.cpp, HidInputProbe.cpp, KeyStoreProbe.cpp, BootProcessProbe.c and BootUiTraceProbe.c, copies of
  UniXNU-sheng package/ (d92e799); LegacyDisplayProbe.cpp without the cpufreq, GPU power and touch probe calls.
Left out: BootLog.c and BootReset.c (scripts/build-boot-progress.py links fajita's own), and sheng's TouchProbe.cpp with the Novatek
and GENI SPI libraries, CpuFreqProbe.cpp and GpuPowerProbe.cpp (SM8550 hardware). The touch snapshot NativeHidInput reads
(native_touch_seq/count/xs/ys/ids, native_touch_set_sink) is defined by libFajitaTouch.a (scripts/build-fajita-touch.py, force-loaded
through CONFIG_XNU_EXTRA_LD_ARCHIVES), which writes it only with fajita-touch=4. The other diagnostics that
BootProgress.c reads from sheng's touch, Adreno and cpufreq code stay the weak stand-ins of kits/fajita/BootProgressStubs.c.

The 21H461 kernelcache addresses below are sheng's; fajita prelinks the same kernelcache (board.toml [kernel.prelink]
kernelcache_sha256 f18b4df8..., slide -0x1000000), and every address is checked against the instruction bytes of the candidate.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/native-legacy-display'
OUT.mkdir(parents=True, exist_ok=True)
KIT = ROOT / 'kits/fajita'
LOGS = ROOT.parent / 'logs'
sys.path.insert(0, str(ROOT / 'kits/NativePrelink'))
from native_prelink import macho, backed_offset, exports   # noqa: E402


def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=300, **kw)


def require(v, why):
    if not v:
        raise SystemExit(why)


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


# Host tools.
objcopy = shutil.which('llvm-objcopy')
env = dict(os.environ)
if not shutil.which('brew'):
    require(objcopy, 'llvm-objcopy not found on PATH')
    shim = OUT / 'host-bin'
    shim.mkdir(exist_ok=True)
    (shim / 'brew').write_text('#!/bin/sh\n# Stand-in for "brew --prefix llvm" (scripts/build-native-hid-input.py): the prefix of PATH\'s llvm-objcopy.\n'
                               f'[ "$1" = --prefix ] && [ "$2" = llvm ] && exec echo {Path(objcopy).resolve().parent.parent}\n'
                               'echo "brew stand-in: only --prefix llvm" >&2; exit 1\n')
    (shim / 'brew').chmod(0o755)
    env['PATH'] = os.pathsep.join([str(shim), env.get('PATH', '')])
else:
    objcopy = objcopy or str(Path(run(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin/llvm-objcopy')

# The reference candidate, under the names audit-mobile-graphics-legacy.py and build-native-hid-input.py look for.
candidate = Path(os.environ.get('FAJITA_DISPLAY_CANDIDATE') or os.environ.get('FAJITA_STORAGE_CANDIDATE') or '')
require(candidate.is_file() and candidate.with_suffix('.json').is_file(),
        'FAJITA_DISPLAY_CANDIDATE (or FAJITA_STORAGE_CANDIDATE) must name a prelinked fajita kernel.macho with its kernel.json beside it')
prelink = ROOT / 'build/native-prelink'
prelink.mkdir(parents=True, exist_ok=True)
for name in ('native-owner-mobile-graphics-candidate', 'native-owner-ufs-cpu-desktop-fajita-candidate'):
    for suffix in ('.macho', '.json'):
        shutil.copyfile(candidate.with_suffix(suffix), prelink / (name + suffix))
analysis = ROOT / 'build/display-analysis/mobile-graphics-legacy-abi.json'
if not analysis.exists():
    analysis.parent.mkdir(parents=True, exist_ok=True)
    analysis.write_text(json.dumps({'runtime_metadata_checkpoint_sha256': '3044c2beff552911605994c0dc7aa73182066cbb5aed0b396999ceeb5a96da2b',
                                    'seeded_by': 'scripts/build-native-legacy-display.py (fajita): recorded hash, no observation'}) + '\n')
print(run(['python3', ROOT / 'scripts/audit-mobile-graphics-legacy.py'], text=True), end='')
audit = json.loads(analysis.read_text())
pure = audit['abstract_slots']
methods = audit['methods']

# The original class ABI header, exactly as sheng generates it.
header = '''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opaque ABI storage for the exact original class, not source-level access
// to its private fields. The actual original metadata supplies its full
// Framebuffer/Service/IOService ancestry. Only qualified base prototypes may
// be called from source; opaque entries are original dispatch targets only.
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
struct OriginalLegacySwapRequest;
class IOMobileFramebufferLegacy : public IOService {
 OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
 unsigned char originalPrivate[0x18a0-sizeof(IOService)];
public:
 // Both fields are qualified by the complete original slot-249 getter.
 // This modifies this object's own mode state, never a vtable or executable.
 void installBootDimensions(uint32_t width,uint32_t height) {
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5808)=width;
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5812)=height;
 }
 const OriginalLegacySwapRequest *pendingSwapRequest() const {
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+5480,sizeof(request));return request;
 }
 // The real normal scanout backend removes its pending head before calling
 // slot 295. Slot 312 is a discard path (terminal state 8), not presentation.
 // The queue is a tail queue (head at +5480, tail pointer at +5488, each request's next at +2960 and previous-link at +2968): taking the head
 // moves the next request, if a client already queued one behind it (int-132: swap 3 arrived during swap 2's GPU copy and the old
 // single-entry form refused to retire swap 2, so every wait on it stayed busy), up to the head. The original queue's reference
 // moves to the current-swap slot for the original completion body.
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+5472,8);memcpy(&head,self+5480,8);memcpy(&tail,self+5488,8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+2960,8);memcpy(&previous,bytes+2968,8);
  if(previous!=self+5480)return false;
  if(next) {
   const void *link=self+5480;
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+2968,&link,8);
  } else if(tail!=bytes+2960)return false;
  memcpy(self+5472,&request,8);
  if(next) {memcpy(self+5480,&next,8);}
  else {const void *empty=nullptr,*emptyTail=self+5480;memcpy(self+5480,&empty,8);memcpy(self+5488,&emptyTail,8);}
  return true;
 }
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 void systemWillShutdown(IOOptionBits) override;
 IOService *probe(IOService *,SInt32 *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
'''
for i in range(168, 358):
    if i == 184:
        header += ' virtual bool isMainDisplay();\n'
    elif i == 188:
        header += ' virtual IOReturn getDotPitch(uint32_t *);\n'
    elif i == 213:
        header += ' virtual IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,uint32_t *)=0;\n'
    elif i == 249:
        header += ' virtual void getDisplayDimensions(uint32_t *,uint32_t *);\n'
    elif i == 290:
        header += ' virtual void kickSwapQueue();\n'
    elif i == 296:
        header += ' virtual bool validateSwap(const OriginalLegacySwapRequest *);\n'
    elif i == 295:
        header += ' virtual void completeCurrentSwap();\n'
    elif i == 337:
        header += ' virtual IOReturn collectCompletionReports(uint32_t,void *,uint32_t *,uint32_t)=0;\n'
    elif i == 288:
        header += ' virtual bool startHardware();\n'
    elif i == 301:
        header += ' virtual bool getRefreshAttributes(uint64_t *,bool);\n'
    elif i in pure:
        header += f' virtual __attribute__((noreturn)) void opaque{i}()=0;\n'
    else:
        header += f' virtual __attribute__((deprecated("Unclassified ABI: never call from source"))) void opaque{i}();\n'
header += '};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)==0x18a0,"Actual original instance extent");\n'
(OUT / 'OriginalLegacyFramebufferABI.h').write_text(header)
print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeLegacyDisplay'], text=True, env=env), end='')

# The compiled vtable: exactly 358 entries, only qualified own methods, every other entry bound to the original.
obj = OUT / 'NativeLegacyDisplay.o'
raw = obj.read_bytes()
image = macho(raw)
st = next(p for c, p, _ in image['commands'] if c == 2)
sy, count, strings, _ = struct.unpack_from('<4I', raw, st + 8)
symbols = []
for i in range(count):
    ix, kind, section, _, va = struct.unpack_from('<IBBHQ', raw, sy + i * 16)
    symbols.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), va, kind))
vtable = next(va for name, va, _ in symbols if name == '__ZTV19NativeLegacyDisplay')
relocations = {}
for segment in image['segments']:
    for sec in segment['sections']:
        if sec['va'] <= vtable < sec['va'] + sec['size']:
            ro, rc = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
            for i in range(rc):
                offset, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                    relocations[sec['va'] + offset] = symbols[bits & 0xffffff][0]
compiled = [relocations.get(vtable + 16 + i * 8) for i in range(358)]
require(all(compiled) and vtable + 16 + 358 * 8 not in relocations, 'Exact owned 358-entry vtable')
reference = prelink / 'native-owner-mobile-graphics-candidate.macho'
old = reference.read_bytes()
old_image = macho(old)
names = {n: v for v, group in exports(old, old_image).items() for n in group}


def read(va, n):
    at = backed_offset(old_image['segments'], va, n, len(old))
    return old[at:at + n]


bindings = {'__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass': 0xfffffff0057865cc,
            '__ZN25IOMobileFramebufferLegacyD2Ev': 0xfffffff005786618,
            '__ZN25IOMobileFramebufferLegacy10gMetaClassE': 0xfffffff006b35940,
            '__ZTV25IOMobileFramebufferLegacy': int(audit['legacy_vptr'], 16) - 16}
OWNED = (0, 1, 7, 14, 84, 86, 87, 141, 155, 184, 188, 288, 290, 296, 301)
owned = []
native = []
for i, name in enumerate(compiled):
    expected = int(methods[i]['address'], 16)
    if name.startswith(('__ZN19NativeLegacyDisplay', '__ZNK19NativeLegacyDisplay')):
        owned.append(i)
        require(i in OWNED or i in pure, 'Only qualified own methods and guarded leaves override')
    elif '25IOMobileFramebufferLegacy' in name:
        require(i not in pure, 'No unresolved original abstract backend target')
        bindings[name] = expected
    else:
        require(i < 168 and names.get(name) == expected, 'Actual inherited native base symbol ' + str(i))
        native.append(i)
require(owned == sorted(list(OWNED) + pure), 'Exact owned method set')
undefined = {n for n, _, k in symbols if k == 1 and 'IOMobileFramebufferLegacy' in n}
require(undefined == set(bindings), 'All and only actual original-class imports qualified')
# The original IOSurface getters, observed through the real original swap request on sheng: each is one load of the named field.
for name, address, field, wide in [
        ('descriptor', 0xfffffff004b9ead0, 48, True), ('width', 0xfffffff004b9ebbc, 88, True),
        ('height', 0xfffffff004b9ebc4, 96, True), ('stride', 0xfffffff004b9ebd4, 104, True),
        ('allocation', 0xfffffff004b9ebfc, 144, True), ('format', 0xfffffff004b9ebac, 128, False),
        ('planes', 0xfffffff004b9ebf4, 152, False)]:
    expected = (0xf9400000 if wide else 0xb9400000) | ((field // (8 if wide else 4)) << 10)
    require(read(address, 8) == struct.pack('<2I', expected, 0xd65f03c0), 'Exact original surface getter ' + name)
    bindings['_native_legacy_surface_' + name] = address
# The original display-refresh notification sender (vsync message type 5 to the clients queued at +5592).
require(read(0xfffffff005796720, 16) == bytes.fromhex('ff0302d1fd7b07a9fdc30191a0831ff8') and
        read(0xfffffff005796830, 4) == bytes.fromhex('08ed4af9') and
        read(0xfffffff00579685c, 16) == bytes.fromhex('e2230091c3008052240080521684ff97') and
        read(0xfffffff005795fbc, 4) == bytes.fromhex('0ab182d2'),
        'Original vsync sender, its +5592 queue and six-word drop-allowed async send')
bindings['_native_legacy_vsync_notify'] = 0xfffffff005796720
# Slot 301 fills the refresh words with the 1/60 s minimum; setDisplayRefreshProperties publishes them as IOMFBDisplayRefresh.
require(int(methods[301]['address'], 16) == 0xfffffff00579ea5c and read(0xfffffff00579ea68, 8) == bytes.fromhex('888888d28888a0f2') and
        read(0xfffffff00579ec54, 4) == bytes.fromhex('08b544f9'),
        'Original slot-301 refresh words with the 1/60 s minimum, called by setDisplayRefreshProperties')
require(read(0xfffffff00579ebb8, 16) == bytes.fromhex('ff0302d1fd7b07a9fdc30191a0031ff8') and
        read(0xfffffff00579ed54, 16) == bytes.fromhex('086540f90173ff90218c0c9100013fd6'),
        'Original setDisplayRefreshProperties publishing IOMFBDisplayRefresh through setProperty')
bindings['_native_legacy_set_refresh_properties'] = 0xfffffff00579ebb8


def branch(pc):
    word = struct.unpack('<I', read(pc, 4))[0]
    require(word & 0xfc000000 == 0x94000000, 'Actual protected base call')
    value = word & 0x3ffffff
    return pc + 4 * (value - (1 << 26) if value & (1 << 25) else value)


require(branch(0xfffffff0057865fc) == 0xfffffff00577b510 and branch(0xfffffff005786630) == 0xfffffff00577b55c,
        'Original protected Legacy C2/D2 really call their original Service bases')
require(read(0xfffffff005787ef8, 16) == bytes.fromhex('080040f9088144f900013fd6e0000037'),
        'Original bool start_hardware() call and result check')
getter = int(methods[249]['address'], 16)
for offset, field in [(0x30, 5808), (0x58, 5812)]:
    require(struct.unpack('<I', read(getter + offset, 4))[0] == 0xb9400000 | ((field // 4) << 10) | (8 << 5) | 8,
            'Actual original width/height getter fields')
require(int(methods[295]['address'], 16) == 0xfffffff005795b88 and
        read(0xfffffff005795b98, 16) == bytes.fromhex('e00740f9280080520101001272d3ff97'),
        'Actual original slot-295 passes true to the normal current completion body')
require(read(0xfffffff00578aa7c, 24) == bytes.fromhex('080040f9084545f9030080d2e20303aa0400805200013fd6'),
        'Original completion calls optional report slot 337 with null outputs and zero limit')
require(read(0xfffffff005777e9c, 16) == bytes.fromhex('080040f908e142f900013fd6c0000036'),
        'Original isMainDisplay dispatch uses only self and tests boolean bit zero')
asm = OUT / 'original-bindings.S'
asm.write_text(''.join(f'.globl {name}\n.set {name}, {va:#x}\n' for name, va in sorted(bindings.items())))
sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True, env=env).strip()
run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-c', asm, '-o', OUT / 'original-bindings.o'], env=env)


def code(data, info):
    sec = next(s for g in info['segments'] for s in g['sections'] if s['name'] == '__text')
    return data[sec['fileoff']:sec['fileoff'] + sec['size']]


def late_init(directory, name):
    """Drop the object's static initializer registration and make it callable: the board probe runs it when the class registers."""
    source, late = directory / f'{name}.o', directory / f'{name}.late.o'
    run([objcopy, '--remove-section=__DATA,__mod_init_func', f'--globalize-symbol=__GLOBAL__sub_I_{name}.cpp', source, late])
    require(code(source.read_bytes(), macho(source.read_bytes())) == code(late.read_bytes(), macho(late.read_bytes())),
            f'Owned {name} instructions unchanged by late init staging')
    return late


late = late_init(OUT, 'NativeLegacyDisplay')

# Board probes. C++ with the IOKit flags of IOPlatformExpert.cpp, C with the osfmk flags of startup.c, as sheng's script.
r = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
cpp = []
args = r['arguments']
i = 0
placeholder_src, placeholder_obj = '@SOURCE@', '@OBJECT@'
while i < len(args):
    value = args[i]
    if value == r['file']:
        cpp.append(placeholder_src)
    elif value == '-o':
        cpp += ['-o', placeholder_obj]
        i += 1
    elif value == '-Xclang' and args[i + 1].startswith('-W'):
        i += 1
    elif not value.startswith('-W'):
        cpp.append(value)
    i += 1
cpp += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator',
        '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
        # As scripts/build-arm-driver.py: upstream clang's -Wextra has it and XNU's kalloc_type macros trip it.
        '-Wno-unknown-warning-option', '-Wno-missing-designated-field-initializers',
        '-I', str(ROOT / '.cache/xnu-work/osfmk')]
pr = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json').read_text().strip().rstrip(','))
c = []
pa = pr['arguments']
j = 0
while j < len(pa):
    value = pa[j]
    if value == pr['file']:
        c.append(placeholder_src)
    elif value in ('-o', '-MF', '-MT', '-MJ'):
        if value == '-o':
            c += ['-o', placeholder_obj]
        j += 1
    elif value == '-Xclang' and pa[j + 1].startswith('-W'):
        j += 1
    elif not value.startswith('-W'):
        c.append(value)
    j += 1
c += ['-Wall', '-Wextra', '-Werror', '-mgeneral-regs-only', '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(KIT)]


def compile_with(template, record, source, extra=()):
    obj = OUT / (source.stem + '.o')
    command = [str(source) if a == placeholder_src else str(obj) if a == placeholder_obj else a for a in template] + list(extra)
    run(command, cwd=record['directory'], env=env)
    return obj


probes = [KIT / 'LegacyDisplayProbe.cpp', KIT / 'HidInputProbe.cpp', KIT / 'KeyStoreProbe.cpp']
probe_objs = [compile_with(cpp, r, source) for source in probes]
c_sources = [KIT / 'BootProcessProbe.c', KIT / 'BootUiTraceProbe.c']
probe_objs += [compile_with(c, pr, source) for source in c_sources]

# The drivers: NativeHidInput through UniXNU's qualification of the original IOHIDDevice, then NativeKeyStoreEvents and the presenter.
print(run(['python3', ROOT / 'scripts/build-native-hid-input.py'], text=True, env=env), end='')
hid_dir = ROOT / 'build/native-hid-input'
print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeKeyStoreEvents'], text=True, env=env), end='')
keyevents_late = late_init(ROOT / 'build/native-keystore-events', 'NativeKeyStoreEvents')
print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeGOPPresenter'], text=True, env=env), end='')
gop_late = late_init(ROOT / 'build/native-gop-presenter', 'NativeGOPPresenter')

members = [late, OUT / 'original-bindings.o', *probe_objs, hid_dir / 'NativeHidInput.late.o', hid_dir / 'hid-original-bindings.o',
           keyevents_late, gop_late]
archive = OUT / 'libNativeLegacyDisplay.late.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, *members], env=env)
# The kernel link force-loads the archive next to libBootProgress.a; their strong symbols must not collide.
def strong_symbols(path):
    return {line.split()[-1] for line in run(['xcrun', 'nm', '-gUm', path], text=True, env=env).splitlines()
            if ' external ' in line and ' weak external ' not in line}


defined = {}
for member in members:
    for name in strong_symbols(member):
        require(name not in defined, f'{name} defined by both {defined.get(name)} and {member.name}')
        defined[name] = member.name
# Its weak stand-ins (kits/fajita/BootProgressStubs.c) give way to these definitions; a strong symbol there would collide.
progress = ROOT / 'build/boot-progress/libBootProgress.a'
if progress.is_file():
    clash = sorted(set(defined) & strong_symbols(progress))
    require(not clash, 'Strong symbols of libBootProgress.a defined again: ' + ', '.join(clash[:8]))

sources = [ROOT / 'drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp', ROOT / 'drivers/NativeMobileDisplay/NativeLegacyRequest.h',
           ROOT / 'drivers/NativeMobileDisplay/NativeLegacyScale.h', ROOT / 'drivers/NativeMobileDisplay/FajitaDisplayLogical.h',
           ROOT / 'drivers/NativeMobileDisplay/NativeGOPPresenter.cpp', ROOT / 'drivers/NativeMobileDisplay/NativeGOPWire.h',
           *probes, *c_sources, KIT / 'BootUiTraceCore.h', KIT / 'BootUiTraceDetails.h',
           ROOT / 'drivers/NativeHidInput/NativeHidInput.cpp', ROOT / 'scripts/build-native-hid-input.py',
           ROOT / 'drivers/NativeKeyStoreEvents/NativeKeyStoreEvents.cpp', Path(__file__).resolve()]
report = dict(
    source_sha256={str(p.relative_to(ROOT)) if p.is_relative_to(ROOT) else str(p): sha(p) for p in sources},
    generated_abi_sha256=sha(OUT / 'OriginalLegacyFramebufferABI.h'),
    original_candidate_sha256=sha(reference), original_audit_sha256=sha(analysis),
    compiled_object_sha256=sha(obj), late_object_sha256=sha(late), archive_sha256=sha(archive),
    members=[m.name for m in members],
    bindings={n: hex(v) for n, v in bindings.items()}, virtual_entries=358, owned_entries=owned, inherited_native_entries=native,
    guarded_backend_leaves=[i for i in pure if i not in (213, 337)], original_instructions_modified=False,
    left_out=['BootLog.c', 'BootReset.c', 'TouchProbe.cpp', 'GeniSpi.c', 'NvtProtocol.c', 'NvtTouchCore.c', 'NvtFirmware.c',
              'CpuFreqProbe.cpp', 'GpuPowerProbe.cpp'],
    driver_initialized=False, display_presented=False, desktop=False)
(OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
LOGS.mkdir(parents=True, exist_ok=True)
shutil.copyfile(OUT / 'build-report.json', LOGS / 'native-legacy-display-build-report.json')
print(f'PASS: fajita Legacy display archive {archive.name} ({len(members)} members), original Legacy and HID bindings qualified '
      f'against candidate {report["original_candidate_sha256"][:16]}; runtime lifecycle unqualified')
