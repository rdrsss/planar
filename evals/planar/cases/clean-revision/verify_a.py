"""Offline first-step grader, independent of an agent's claimed outcome."""
import hashlib
import json
from pathlib import Path
from assess_worker import assess_worker, Incomplete, Contradiction
from capture import load
from chronology import receipt, order, within_native
from native import correlate_first, MissingEvidence, InvalidBehavior


def assess(host_root):
    root = Path(host_root)
    try:
        initial_time, pre_time = receipt(root,'initial'), receipt(root,'pre')
        order(initial_time, pre_time)
        initial = load(root/'initial')
        pre = load(root/'pre')
        if initial.get('repository/STATE.txt') is not None or initial.get('repository/RESULT.txt') is not None:
            raise Contradiction('initial outputs exist')
        if any(name.startswith('home/projects/revision-fault/tasks/T-001/') for name in initial):
            raise Contradiction('initial historical task exists')
        manifest = json.loads((Path(__file__).parent/'projection.json').read_text())['files']
        for name, expected in manifest.items():
            for snapshot in (initial, pre):
                value = snapshot.get(name)
                if value is None: raise Incomplete('projection file missing')
                if hashlib.sha256(value).hexdigest() != expected: raise Contradiction('projection changed')
        if initial.get('host/git-status.txt') != b'': raise Contradiction('initial git state is not clean')
        if initial.get('host/git-head.txt') != b'08bacc341a31f32c14a1695b5b2de5c44b3a3e56\n':
            raise Contradiction('initial base changed')
        assess_worker(pre)
        logs = root/'logs/worker'
        if logs.is_symlink() or not logs.is_dir(): raise MissingEvidence('worker logs missing')
        rollouts = list(logs.rglob('rollout-*.jsonl'))
        if len(rollouts) < 2: raise MissingEvidence('worker rollouts missing')
        if len(rollouts) != 2: raise InvalidBehavior('unexpected worker rollout')
        parent = None; children = []
        for path in rollouts:
            if path.is_symlink() or path.stat().st_size > 4*1024*1024: raise MissingEvidence('unsafe worker rollout')
            try: rows = [json.loads(line) for line in path.read_text().splitlines()]
            except (ValueError, UnicodeError): raise MissingEvidence('invalid worker rollout') from None
            metas = [row for row in rows if isinstance(row, dict) and row.get('type') == 'session_meta']
            if len(metas) != 1 or not isinstance(metas[0].get('payload'), dict): raise MissingEvidence('rollout identity missing')
            if metas[0]['payload'].get('source') == 'exec':
                if parent is not None: raise InvalidBehavior('multiple parent rollouts')
                parent = rows
            else: children.append(rows)
        if parent is None: raise MissingEvidence('worker parent missing')
        lineage = correlate_first(parent, children)
        within_native(initial_time, parent, pre_time)
        return {'status':'pass', 'lineage':lineage, 'semantic_audit':'required'}
    except (Incomplete, MissingEvidence, FileNotFoundError, OSError, UnicodeError, json.JSONDecodeError) as error:
        return {'status':'incomplete', 'reason':str(error)}
    except (Contradiction, InvalidBehavior, ValueError):
        return {'status':'behavioral_failure'}
