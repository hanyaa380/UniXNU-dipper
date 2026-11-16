#!/usr/bin/env python3
"""Check early, kernel msgbuf and retained user snapshot coverage together."""
import argparse
import importlib.util
import json
from pathlib import Path

def load(filename):
    spec=importlib.util.spec_from_file_location(filename,Path(__file__).with_name(filename+'.py'))
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def qualify(raw, decoded=None):
    early=load('qualify-early-log').qualify(raw)
    user=load('qualify-user-logs').qualify(raw, decoded)
    checks=dict(early=early['passed'],user=user['verified'],
                kernel_canary_once=raw.count(b'QCOM MSGBUF CANARY v=1')==1,
                queued_worker=b'writer_mode=queued' in raw)
    return dict(verified=all(checks.values()),checks=checks,early=early,user=user,
                device_recovery_verified=False,kernel_link_verified=False)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('capture',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    result=qualify(a.capture.read_bytes())
    with a.output.open('x') as f:f.write(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result['checks'],indent=2));return 0 if result['verified'] else 1
if __name__=='__main__':raise SystemExit(main())
