"""Check host receipts, not unauthenticated agent chronology claims."""
import hashlib
import json
from pathlib import Path


def receipt(root, stage):
    root = Path(root)
    path = root/(stage+'-receipt.json')
    manifest = root/stage/'snapshot-manifest.json'
    if path.is_symlink() or manifest.is_symlink(): raise ValueError('unsafe snapshot receipt')
    if not path.is_file() or not manifest.is_file(): raise FileNotFoundError('snapshot receipt missing')
    data = json.loads(path.read_text())
    if data.get('stage') != stage or data.get('manifest_sha256') != hashlib.sha256(manifest.read_bytes()).hexdigest():
        raise ValueError('snapshot receipt mismatch')
    if any(type(data.get(name)) is not int or data[name] <= 0 for name in ('wall_ns','monotonic_ns')):
        raise ValueError('invalid snapshot time')
    return data


def order(first, second):
    if any(type(value.get(name)) is not int or value[name] <= 0 for value in (first,second) for name in ('wall_ns','monotonic_ns')):
        raise ValueError('invalid evidence timestamp')
    if any(first[name] >= second[name] for name in ('wall_ns','monotonic_ns')):
        raise ValueError('host evidence order mismatch')


def native_bounds(rows):
    from datetime import datetime
    if not isinstance(rows, list): raise ValueError('invalid native rollout')
    starts = [row for row in rows if isinstance(row,dict) and row.get('type') == 'session_meta']
    ends = [row for row in rows if isinstance(row,dict) and row.get('type') == 'event_msg' and isinstance(row.get('payload'),dict) and row['payload'].get('type') == 'task_complete']
    if len(starts) != 1 or len(ends) != 1: raise ValueError('native boundary missing')
    def stamp(row):
        try: value = datetime.fromisoformat(row['timestamp'].replace('Z','+00:00'))
        except (KeyError, AttributeError, TypeError, ValueError): raise ValueError('invalid native timestamp') from None
        if value.tzinfo is None: raise ValueError('native timestamp lacks zone')
        return int(value.timestamp()*1_000_000_000)
    return stamp(starts[0]), stamp(ends[0])


def within_native(first, rows, second):
    start, end = native_bounds(rows)
    if not (first['wall_ns'] <= start <= end <= second['wall_ns']):
        raise ValueError('native and host evidence order mismatch')
