#!/usr/bin/env python3
"""Measure cold-entry to USB-ready coverage without changing the timer epoch."""
import argparse
import hashlib
import json
from pathlib import Path
import re

STAGES = ('start_first_cpu', 'arm_init_rebased', 'cpu_data_ready', 'platform_ready',
          'topology_ready', 'bootstrap_thread_ready', 'rtclock_ready',
          'startup_bootstrap_ready', 'processor_ready', 'arm_vm_init_begin',
          'arm_vm_init_returned', 'serial_init_returned', 'usb_init_begin',
          'usb_init_end', 'kprintf_startup_ready')
RECORD = re.compile(rb'QCOM EARLY stage=([a-z_]+) counter_ticks=(\d+) '
                    rb'entry_ticks=(\d+) freq_hz=(\d+) delta_us=(\d+|invalid)\r?\n')


def qualify(raw):
    records = []
    for match in RECORD.finditer(raw):
        stage, now, entry, frequency, delta = match.groups()
        record = dict(stage=stage.decode(), counter_ticks=int(now), entry_ticks=int(entry),
                      freq_hz=int(frequency), delta_us=None if delta == b'invalid' else int(delta),
                      byte_offset=match.start())
        records.append(record)
    ordered = [r['stage'] for r in records] == list(STAGES)
    valid = bool(records) and all(0 < r['freq_hz'] <= 1000000000 and
        r['entry_ticks'] < 2**64-1 and r['entry_ticks'] <= r['counter_ticks'] < 2**64 and
        r['delta_us'] == (r['counter_ticks']-r['entry_ticks'])*1000000//r['freq_hz']
        for r in records)
    consistent = bool(records) and len({(r['entry_ticks'], r['freq_hz']) for r in records}) == 1
    monotonic = bool(records) and all(a['counter_ticks'] <= b['counter_ticks'] for a,b in zip(records, records[1:]))
    buffered = bool(records) and b'QCOM XNU USB CDC ACM v1' in raw and records[0]['byte_offset'] < raw.index(b'QCOM XNU USB CDC ACM v1')
    checks = dict(all_stages_in_order=ordered, valid_counter_conversion=valid,
                  consistent_epoch=consistent, monotonic=monotonic,
                  cold_entry_replayed_before_usb_banner=buffered,
                  no_panic=b'panic(cpu' not in raw,
                  no_reported_transport_loss=b'QCOM USB LOSS' not in raw)
    result = dict(scope='Cold start_first_cpu through KPRINTF startup; firmware before kernel entry is outside coverage',
                  raw_bytes=len(raw), raw_sha256=hashlib.sha256(raw).hexdigest(),
                  checks=checks, passed=all(checks.values()), stages=records)
    if valid and consistent:
        result['entry_counter_seconds'] = records[0]['entry_ticks']/records[0]['freq_hz']
        result['entry_to_last_stage_us'] = records[-1]['delta_us']
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trial', type=Path)
    args = parser.parse_args()
    result = qualify((args.trial/'capture/xnu-usb.log').read_bytes())
    (args.trial/'early-log-qualification.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k != 'stages'}, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
