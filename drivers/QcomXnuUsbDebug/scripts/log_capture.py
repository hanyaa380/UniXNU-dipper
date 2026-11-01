"""Post-capture logging analysis shared by the collector and offline CLI."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path


def load(name):
    spec = importlib.util.spec_from_file_location('qud_' + name, Path(__file__).with_name(name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def verify_capture(directory):
    directory = Path(directory)
    metadata = json.loads((directory / 'capture.json').read_text())
    raw = (directory / 'xnu-usb.log').read_bytes()
    identity = (metadata.get('bytes') == metadata.get('synced_bytes') == len(raw) and
                metadata.get('sha256') == hashlib.sha256(raw).hexdigest())
    decoder = load('decode-user-logs')
    decoded = decoder.decode(raw)
    result = load('qualify-logs').qualify(raw, decoded)
    result['checks']['synced_capture_identity'] = identity
    result['checks']['capture_succeeded'] = metadata.get('status') not in (None, 'capture-failed')
    result['verified'] = all(result['checks'].values())
    destination = directory / 'logs'
    decoder.publish(decoded, destination)
    report = destination / 'qualification.json'
    with report.open('x') as output:
        output.write(json.dumps(result, indent=2) + '\n')
    return dict(verified=result['verified'], report=str(report),
                checks=result['checks'], raw_sha256=result['user']['raw_sha256'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture_directory', type=Path)
    args = parser.parse_args()
    result = verify_capture(args.capture_directory)
    print(json.dumps(result, indent=2))
    return 0 if result['verified'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
