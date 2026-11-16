"""Canonical package inventory for the T-044 smoke; performs no model calls."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path


def inventory(root: Path) -> dict:
    root = root.resolve(strict=True)
    entries = []
    for path in sorted(root.rglob('*')):
        if path.is_symlink():
            raise ValueError(f'symlink in candidate: {path.relative_to(root)}')
        if path.is_dir():
            continue
        if not path.is_file():
            raise ValueError(f'non-regular candidate entry: {path.relative_to(root)}')
        entries.append({'path': path.relative_to(root).as_posix(),
                        'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    if not entries:
        raise ValueError('empty candidate')
    data = json.dumps(entries, ensure_ascii=True, sort_keys=True, separators=(',', ':')).encode()
    return {'algorithm': 'sha256-json-inventory-v1', 'sha256': hashlib.sha256(data).hexdigest(), 'files': entries}


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path)
    parser.add_argument('--compare', type=Path)
    args = parser.parse_args()
    result = inventory(args.root)
    if args.compare and result != json.loads(args.compare.read_text()):
        raise SystemExit('candidate digest mismatch')
    print(json.dumps(result, indent=2, sort_keys=True))
