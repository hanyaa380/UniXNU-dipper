#!/usr/bin/env python3
"""Build the optional iOS16 root collector; signing/deployment belong to the consumer."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--clang',type=Path,required=True)
    p.add_argument('--sdk',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();out=a.output.resolve()
    if ROOT==out or ROOT in out.parents or not any(x in ('tmp','.cache') for x in out.parts):
        p.error('output must be a new directory outside source under tmp/ or .cache/')
    if not (a.sdk/'SDKSettings.plist').is_file():p.error('SDK unavailable')
    out.mkdir(parents=True,exist_ok=False)
    cmd=[str(a.clang.absolute()),'--target=arm64-apple-ios16.0','-isysroot',str(a.sdk.resolve()),
         '-std=c++17','-fobjc-arc','-fblocks','-O2','-Wall','-Wextra','-Werror',
         '-I'+str(ROOT/'include'),'-I'+str(ROOT/'userland'),
         str(ROOT/'userland/QcomOsLogExport.mm'),str(ROOT/'userland/main.mm'),
         '-framework','Foundation','-lz','-o',str(out/'qcom-usb-log')]
    with (out/'build.log').open('wb') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
    inputs=[*ROOT.glob('userland/*'),ROOT/'include/QcomUserLogWire.h']
    report={'command':cmd,'device_verified':False,'signed_for_deployment':False,
            'binary_sha256':hashlib.sha256((out/'qcom-usb-log').read_bytes()).hexdigest(),
            'source_sha256':{str(f.relative_to(ROOT)):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs if f.is_file()}}
    (out/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(out/'qcom-usb-log')
if __name__=='__main__':main()
