"""Bounded target-only lineage; this does not verify task work or issue reward."""
import json
from pathlib import Path


def load_target(logs):
    from native import _meta, _time, MissingEvidence, InvalidBehavior
    root = Path(logs)
    if root.is_symlink() or not root.is_dir(): raise MissingEvidence('target logs missing')
    if any(path.is_symlink() for path in root.rglob('*')): raise MissingEvidence('unsafe target log path')
    files = list(root.rglob('rollout-*.jsonl'))
    if len(files) < 4: raise MissingEvidence('target rollout missing')
    if len(files) != 4: raise InvalidBehavior('unexpected target rollout')
    parents, children = [], []
    for path in files:
        if path.is_symlink() or path.stat().st_size > 4*1024*1024: raise MissingEvidence('unsafe target rollout')
        try: rows = [json.loads(line) for line in path.read_text().splitlines()]
        except (ValueError,UnicodeError): raise MissingEvidence('invalid target rollout') from None
        meta = _meta(rows)
        if meta['payload'].get('source') == 'exec': parents.append(rows)
        else: children.append(rows)
    if len(parents) != 1: raise MissingEvidence('target parent missing or ambiguous')
    children.sort(key=lambda rows:_time(_meta(rows)))
    return parents[0], children


def correlate_target(logs, post_receipt, final_receipt, source_lineage):
    from native import _correlate, InvalidBehavior
    from chronology import within_native, order
    parent, children = load_target(logs)
    result = _correlate([parent],children,[('gpt-6.1-sol','gpt-6.1-terra','gpt-6.1-sol')])
    if set(result['parents']+result['children']) & set(source_lineage['parents']+source_lineage['children']):
        raise InvalidBehavior('source and target identities overlap')
    order(post_receipt,final_receipt); within_native(post_receipt,parent,final_receipt)
    return result
