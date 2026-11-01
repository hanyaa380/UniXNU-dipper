#!/usr/bin/env python3
"""Run logging source models in a new generated workspace, never on a device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--xnu',type=Path,required=True,help='pinned pristine Git source checkout (read-only)')
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();out=a.output.resolve()
    if ROOT==out or ROOT in out.parents or not any(x in ('tmp','.cache') for x in out.parts):
        p.error('new output must be outside source under tmp/ or .cache/')
    out.mkdir(parents=True,exist_ok=False)
    os.environ['QUD_TEST_XNU']=str(a.xnu.resolve())
    tempfile.tempdir=str(out)
    suite=unittest.TestSuite()
    for pattern in ('test_msgbuf.py','test_early_log.py','test_ios16_log_store.py','test_user_logs.py','test_log_native.py'):
        suite.addTests(unittest.defaultTestLoader.discover(str(ROOT/'tests'),pattern=pattern))
    with (out/'tests.log').open('w') as log:result=unittest.TextTestRunner(stream=log,verbosity=2).run(suite)
    files=[f for sub in ('kernel','include','userland','scripts','tests','patches') for f in (ROOT/sub).rglob('*') if f.is_file() and f.name!='.DS_Store']
    report={'tests':result.testsRun,'failures':len(result.failures),'errors':len(result.errors),
            'passed':result.wasSuccessful(),'device_verified':False,
            'source_sha256':{str(f.relative_to(ROOT)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files}}
    (out/'logging-check-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='source_sha256'},indent=2))
    return 0 if result.wasSuccessful() else 1
if __name__=='__main__':raise SystemExit(main())
