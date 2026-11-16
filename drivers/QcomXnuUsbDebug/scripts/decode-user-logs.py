#!/usr/bin/env python3
"""Verify framed USB user logs; preserve raw captures and only publish valid snapshots."""
from collections import Counter
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import zlib

FRAME = re.compile(rb'QCOM_ULOG v=1 sid=([0-9a-f]{16}) stream=(\d+) seq=(\d+) total=(\d+) off=(\d+) bytes=(\d+) crc=([0-9a-f]{8}) flags=(\d+) data=([0-9a-f]*)')
MAX_BYTES = 4 * 1024 * 1024


def page_groups(records):
    groups = {}
    for record in records:
        if 'page' in record:
            key = (record['session'], record['page']['pass'])
            groups.setdefault(key, []).append(record)
    result = []
    for (session, number), pages in groups.items():
        pages.sort(key=lambda item: item['page']['page'])
        errors, total, processes, texts = [], 0, Counter(), []
        for index, record in enumerate(pages, 1):
            page = record['page']
            if (page['page'] != index or index > 8 or page['first'] != total + 1 or
                    page['count'] > 60000 or page['total'] != total + page['count'] or
                    page['final'] != int(index == len(pages)) or
                    record.get('entries') != [page['count']] or not record['complete'] or
                    record.get('coverage_errors') or record.get('text_bytes', 0) > 8388608):
                errors.append('invalid_or_incomplete_page_'+str(index))
            total = page['total']
            processes.update(record.get('processes', {}))
            texts.append(record.get('text', ''))
        result.append({'session': session, 'pass': number, 'flags': 2,
                       'streams': [p['stream'] for p in pages], 'pages': len(pages),
                       'complete': not errors, 'coverage_errors': errors,
                       'entries': [total], 'processes': dict(processes),
                       'canary_seen': any(p.get('canary_seen') for p in pages),
                       'canary_nonces': sorted({n for p in pages for n in p.get('canary_nonces', [])}),
                       'text': ''.join(texts)})
    return result


def decode(raw):
    streams = {}
    errors = []
    for line_number, line in enumerate(raw.splitlines(), 1):
        if b'QCOM_ULOG ' not in line:
            continue
        m = FRAME.search(line)
        if not m:
            errors.append({'line': line_number, 'error': 'malformed_frame'}); continue
        sid, stream, seq, total, off, count, crc, flags, data = m.groups()
        stream, seq, total, off, count, flags = map(int, (stream, seq, total, off, count, flags))
        payload = bytes.fromhex(data.decode()) if len(data) % 2 == 0 else b''
        valid = (0 < stream <= 0xffffffff and 0 < total <= MAX_BYTES and 0 < count <= 1024 and
                 off % 1024 == 0 and seq == off // 1024 and off + count <= total and
                 (count == 1024 or off + count == total) and flags in (1, 2) and
                 len(payload) == count and zlib.crc32(payload) == int(crc, 16) and m.end() == len(line))
        if not valid:
            errors.append({'line': line_number, 'error': 'bounds_or_crc', 'stream': stream}); continue
        key = (sid.decode(), stream)
        state = streams.setdefault(key, {'session': key[0], 'stream': stream, 'total': total,
                                         'flags': flags, 'parts': {}, 'errors': []})
        if state['total'] != total or state['flags'] != flags:
            state['errors'].append('metadata_changed'); continue
        if off in state['parts'] and state['parts'][off] != payload:
            state['errors'].append('conflicting_duplicate'); continue
        state['parts'][off] = payload
    records = []
    for state in streams.values():
        offset = 0
        body = bytearray()
        for at, data in sorted(state.pop('parts').items()):
            if at != offset:
                state['errors'].append('gap_at_'+str(offset)); break
            body.extend(data); offset += len(data)
        state['received'] = offset
        state['complete'] = offset == state['total'] and not state['errors']
        if state['complete']:
            state['sha256'] = hashlib.sha256(body).hexdigest()
            if state['flags'] == 1:
                state['status'] = body.decode('utf-8', 'replace')
            else:
                try:
                    with gzip.GzipFile(fileobj=io.BytesIO(body)) as stream:
                        text = stream.read(16 * 1024 * 1024 + 1)
                    if len(text) > 16 * 1024 * 1024:
                        raise ValueError('decoded_text_limit')
                    state['text'] = text.decode('utf-8', 'replace')
                    state['text_bytes'] = len(text)
                    state['coverage_errors'] = [marker for marker in ('# framework load failed:', '# no store:', '# no entries:', '# TRUNCATED')
                                                if marker in state['text']]
                    state['canary_seen'] = 'QCOM USERLOG CANARY' in state['text']
                    state['canary_nonces'] = sorted(set(re.findall(r'QCOM USERLOG CANARY pid=\d+ nonce=([0-9a-f]{16})\b',state['text'])))
                    state['entries'] = [int(x) for x in re.findall(r'^# (\d+) entries$', state['text'], re.M)]
                    state['processes'] = dict(Counter(re.findall(r'^\s*[0-9.]+\s+\d+\s+(\S+)\s',state['text'],re.M)))
                    state['paged'] = bool(re.search(r'^# qcom oslog pass \d+ page=', state['text'], re.M))
                    if state['paged']:
                        headers = re.findall(r'^# qcom oslog pass (\d+) page=(\d+) first_entry=(\d+) ', state['text'], re.M)
                        footers = re.findall(r'^# qcom_page pass=(\d+) page=(\d+) first=(\d+) count=(\d+) final=([01]) total=(\d+)$', state['text'], re.M)
                        if len(headers) != 1 or len(footers) != 1 or headers[0] != footers[0][:3]:
                            state['coverage_errors'].append('# invalid page metadata')
                        else:
                            state['page'] = dict(zip(('pass', 'page', 'first', 'count', 'final', 'total'), map(int, footers[0])))
                except (OSError, EOFError, ValueError, zlib.error) as error:
                    state['complete'] = False; state['errors'].append('gzip:'+str(error))
        records.append(state)
    gaps=[]
    for sid in sorted({r['session'] for r in records}):
        previous=0
        for number in sorted({r['stream'] for r in records if r['session']==sid}):
            if number!=previous+1:gaps.append({'session':sid,'first_missing':previous+1,'last_missing':number-1})
            previous=number
    return {'format': 'qcom-user-log-decode-v1', 'raw_sha256': hashlib.sha256(raw).hexdigest(),
            'raw_bytes': len(raw), 'frame_errors': errors, 'stream_gaps': gaps, 'streams': records,
            'groups': page_groups(records)}


def publish(result, output):
    """Publish into a fresh directory without consuming the decoded text."""
    output.mkdir(parents=True, exist_ok=False)
    groups = [r for r in result['groups'] if r['complete']]
    if groups:
        (output/'latest-oslog.txt').write_text(groups[-1]['text'])
        result['latest_complete_group'] = [groups[-1]['session'], groups[-1]['pass']]
    result = dict(result, streams=[{k:v for k,v in r.items() if k != 'text'} for r in result['streams']],
                  groups=[{k:v for k,v in r.items() if k != 'text'} for r in result['groups']])
    (output/'manifest.json').write_text(json.dumps(result, indent=2)+'\n')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = decode(args.capture.read_bytes())
    publish(result, args.output)
    valid = [r for r in result['streams'] if r['flags'] == 2 and r['complete']]
    groups = [r for r in result['groups'] if r['complete']]
    print(json.dumps({'streams': len(result['streams']), 'complete_snapshots': len(valid),
                      'complete_groups': len(groups),
                      'frame_errors': len(result['frame_errors']), 'output': str(args.output)}, indent=2))
    return 0 if groups and not result['frame_errors'] and not result['stream_gaps'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
