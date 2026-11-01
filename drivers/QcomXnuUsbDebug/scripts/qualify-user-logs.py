#!/usr/bin/env python3
"""Qualify retained system snapshots independently of device recovery evidence."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
spec = importlib.util.spec_from_file_location('qcom_decode', Path(__file__).with_name('decode-user-logs.py'))
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)

def usable_system_snapshot(record, expected_canaries):
    # A collector-only canary can also come from a process-scoped store. Require
    # retained entries from at least one concrete external process as evidence.
    return (record['flags']==2 and record['complete'] and
            not record['coverage_errors'] and record['canary_seen'] and
            bool(set(record['canary_nonces']) & expected_canaries) and
            any(record['entries']) and
            any(name not in ('qcom-usb-log','?') for name in record['processes']))

def completed_system_groups(decoded):
    canaries, budgets = {}, set()
    for record in decoded['streams']:
        status = record.get('status', '')
        match = re.search(r'^collector_started .* canary_nonce=([0-9a-f]{16})\b', status)
        if match:canaries.setdefault(record['session'],set()).add(match.group(1))
        if re.match(r'^collector_memory_budget stage=verified error=0 active_mb=128 active_attr=1 inactive_mb=128 inactive_attr=1\b', status):
            budgets.add(record['session'])
    return [g for g in decoded['groups'] if g['session'] in budgets and
            usable_system_snapshot(g, canaries.get(g['session'], set()))]


def qualify(raw, decoded=None):
    if decoded is None:
        decoded = decoder.decode(raw)
    groups = completed_system_groups(decoded)
    checks = {
        'current_system_snapshot': bool(groups),
        'no_frame_corruption': not decoded['frame_errors'],
        'no_stream_gaps': not decoded['stream_gaps'],
        'no_panic': b'panic(cpu' not in raw,
        'no_reported_loss': b'QCOM USB LOSS' not in raw,
        'no_collector_memory_kill': not re.search(rb'EXC_RESOURCE -> qcom-usb-log\[\d+\] exceeded mem limit:', raw),
    }
    return dict(verified=all(checks.values()), checks=checks,
                raw_sha256=hashlib.sha256(raw).hexdigest(), raw_bytes=len(raw),
                qualified_groups=[{k:v for k,v in g.items() if k != 'text'} for g in groups],
                scope='Retained system-store pages only; kernel coverage, full linking and device recovery require independent evidence')

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    if a.output.exists(): p.error('output already exists')
    result = qualify(a.capture.read_bytes())
    with a.output.open('x') as f: f.write(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))
    return 0 if result['verified'] else 1

if __name__ == '__main__': raise SystemExit(main())
