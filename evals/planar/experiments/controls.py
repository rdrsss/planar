"""Deterministic T-046 control prototype. No model or host dispatch is provided.

Harbor 0.26 JobPlan resolves the repeated native trial matrix. These controls
exercise identity, retention and reporting only; injected outcomes are not
live evaluation evidence. See TS-25.3 and TS-25.5.
"""
from __future__ import annotations

import asyncio
import hashlib
import json
import os
import shutil
from pathlib import Path
from datetime import datetime, timedelta, timezone
from uuid import uuid4

from harbor.job_plan import JobPlan
from harbor.models.job.config import JobConfig


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode()


def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def create(path, value):
    """Write an immutable record; a partial write is deliberately not repaired."""
    data = canonical(value) + b'\n'
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    directory = os.open(Path(path).parent, os.O_RDONLY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def read(path):
    return json.loads(path.read_text())


def native_plan(config):
    """Resolve Harbor's matrix, preserving its repetitions and native identities."""
    cfg = JobConfig.model_validate(config)
    if cfg.n_concurrent_trials != 1 or cfg.retry.max_retries != 0:
        raise ValueError('serial execution and zero native retries required')
    if cfg.n_attempts < 1 or cfg.n_attempts > 100:
        raise ValueError('finite bounded repetitions required')
    plan = asyncio.run(JobPlan.from_config(cfg))
    return plan



def tree_digest(root):
    root = Path(root)
    entries = []
    for path in sorted(root.rglob('*')):
        if path.is_symlink():
            raise ValueError('snapshot contains symlink')
        if path.is_file() and '__pycache__' not in path.parts:
            entries.append((path.relative_to(root).as_posix(), hashlib.sha256(path.read_bytes()).hexdigest()))
    if not entries:
        raise ValueError('empty snapshot')
    return digest(entries)

def initialize(root, config, *, hypothesis, baseline, variants, factors, pins, caps, candidate_source=None, mode='nop', variant_efforts=None):
    root = Path(root)
    if not hypothesis or not baseline or baseline not in variants or len(set(variants)) != len(variants):
        raise ValueError('hypothesis, baseline and unique variants required')
    if not factors or not pins or not all(k in pins and pins[k] for k in ('candidate', 'case', 'verifier', 'image', 'host')):
        raise ValueError('factors and candidate/case/verifier/image/host pins required')
    if not all(isinstance(caps.get(k), int) and 0 < caps[k] <= 86400 for k in ('session_seconds', 'verifier_seconds', 'trial_seconds', 'job_seconds', 'trials')):
        raise ValueError('positive finite caps required')
    if mode not in ('nop', 'codex'):
        raise ValueError('unsupported mode')
    if mode == 'codex':
        import sys
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'adapters'))
        import launch
        if pins.get('case_name') not in launch.CASES or not isinstance(variant_efforts, dict) or set(variant_efforts) != set(variants):
            raise ValueError('Codex case and variant effort mapping required')
        if any(e not in ('medium', 'high') for e in variant_efforts.values()):
            raise ValueError('unsupported Codex effort')
        if pins['case_name'] == 'fresh-interrupted-review' and any(e != 'medium' for e in variant_efforts.values()):
            raise ValueError('recovery case requires medium effort')
        from candidate import inventory
        from harbor.models.task.task import Task
        candidate_path = Path(candidate_source or Path(__file__).resolve().parents[3] / 'plugins/planar').resolve(strict=True)
        case_path = Path(config['tasks'][0]['path']).resolve(strict=True)
        verified_pins = {'candidate': inventory(candidate_path)['sha256'], 'case': launch.case_inventory(case_path),
            'verifier': launch.case_inventory(case_path / 'tests'), 'image': Task(case_path).config.environment.docker_image,
            'host': 'codex-cli 0.169.0'}
        if any(pins.get(k) != v for k, v in verified_pins.items()):
            raise ValueError('declared pins contradict resolved candidate, case, verifier, image or host')
        expected = launch.config_for(config['job_name'], Path(config['jobs_dir']), Path(config['tasks'][0]['path']))
        expected['n_attempts'] = config.get('n_attempts', 1)
        expected['agents'] = [launch.config_for(config['job_name'], Path(config['jobs_dir']), Path(config['tasks'][0]['path']), variant_efforts[v])['agents'][0] for v in variants]
        if JobConfig.model_validate(expected) != JobConfig.model_validate(config):
            raise ValueError('Codex config differs from guarded launcher')
    plan = native_plan(config)
    if len(plan.trial_configs) > caps['trials'] or len(plan.config.agents) != len(variants):
        raise ValueError('trial cap or variant count mismatch')
    if len(plan.task_configs) != 1:
        raise ValueError('prototype accepts one case')
    if mode == 'nop' and any(a.name != 'nop' for a in plan.config.agents):
        raise ValueError('nop controls accept only Harbor nop agents')
    manifest = {'schema': 1, 'kind': 'injected-control-only' if mode == 'nop' else 'codex-experiment', 'variant_efforts': variant_efforts, 'hypothesis': hypothesis,
                'baseline': baseline, 'variants': variants, 'factors': factors, 'pins': pins, 'created_at': datetime.now(timezone.utc).isoformat(),
                'resolved_pins': verified_pins if mode == 'codex' else None, 'pin_provenance': {'candidate': 'snapshot inventory', 'case': 'snapshot inventory', 'verifier': 'snapshot inventory', 'image': 'configured task image; runtime observation unavailable', 'host': 'configured CLI version; runtime observation unavailable'} if mode == 'codex' else None, 'caps': caps, 'deadline_at': (datetime.now(timezone.utc) + timedelta(seconds=caps['job_seconds'])).isoformat(), 'config': plan.config.model_dump(mode='json'),
                'native_plan_id': str(plan.id), 'trials': []}
    # Harbor orders repetitions, tasks, then agents. Keep the exact native order.
    for i, trial in enumerate(plan.trial_configs):
        manifest['trials'].append({'logical_id': f'{variants[i % len(variants)]}:{i // len(variants) + 1}',
                                   'variant': variants[i % len(variants)], 'repetition': i // len(variants) + 1,
                                   'native_trial_name': trial.trial_name})
    manifest['identity'] = digest(manifest)
    root.mkdir(mode=0o700, parents=False, exist_ok=False)
    (root / 'executions').mkdir(mode=0o700)
    case_source = Path(plan.config.tasks[0].path).resolve(strict=True)
    before = tree_digest(case_source)
    shutil.copytree(case_source, root / 'case-snapshot', ignore=shutil.ignore_patterns('__pycache__'))
    if tree_digest(case_source) != before or tree_digest(root / 'case-snapshot') != before:
        raise ValueError('case changed while freezing')
    manifest['case_snapshot_sha256'] = before
    source = Path(candidate_source or Path(__file__).resolve().parents[3] / 'plugins/planar').resolve(strict=True)
    candidate_digest = tree_digest(source)
    shutil.copytree(source, root / 'candidate-snapshot', ignore=shutil.ignore_patterns('__pycache__'))
    if tree_digest(source) != candidate_digest or tree_digest(root / 'candidate-snapshot') != candidate_digest:
        raise ValueError('candidate changed while freezing')
    manifest['candidate_snapshot_sha256'] = candidate_digest
    if mode == 'codex' and (inventory(root / 'candidate-snapshot')['sha256'] != verified_pins['candidate'] or launch.case_inventory(root / 'case-snapshot') != verified_pins['case'] or launch.case_inventory(root / 'case-snapshot/tests') != verified_pins['verifier']):
        raise ValueError('snapshot changed during canonical inventory')
    manifest['identity'] = digest({k: v for k, v in manifest.items() if k != 'identity'})
    create(root / 'manifest.json', manifest)
    return manifest


def validate(root, identity):
    m = read(Path(root) / 'manifest.json')
    expected = m.pop('identity')
    if digest(m) != expected or identity != expected or tree_digest(Path(root) / 'case-snapshot') != m['case_snapshot_sha256'] or tree_digest(Path(root) / 'candidate-snapshot') != m['candidate_snapshot_sha256']:
        raise ValueError('experiment identity changed')
    m['identity'] = expected
    return m


def executions(root):
    rows = []
    for d in sorted((Path(root) / 'executions').iterdir()):
        if not d.is_dir():
            raise ValueError('invalid execution entry')
        start = read(d / 'start.json')
        terminal = read(d / 'terminal.json') if (d / 'terminal.json').exists() else {'status': 'incomplete', 'reason': 'ambiguous interrupted execution'}
        native = read(d / 'native-start.json') if (d / 'native-start.json').is_file() else None
        dispatch = read(d / 'dispatch.json') if (d / 'dispatch.json').is_file() else None
        live_result = read(d / 'native-result.json') if (d / 'native-result.json').is_file() else None
        stop = read(d / 'account-stop.json') if (d / 'account-stop.json').is_file() else None
        resolution = read(d / 'account-stop-resolved.json') if (d / 'account-stop-resolved.json').is_file() else None
        observed = {}
        if dispatch:
            for label, name in (('image', 'planar-image.json'), ('cli', 'planar-cli.json')):
                source = Path(dispatch['evidence']) / name
                observed[label] = read(source) if source.is_file() else None
        rows.append({**start, **terminal, 'native': native, 'dispatch': dispatch, 'live_result': live_result, 'account_stop': stop, 'account_stop_resolution': resolution, 'observed': observed, 'evidence': str(d), 'usage': live_result.get('usage') if live_result else None, 'timing': live_result.get('timing') if live_result else None})
    return rows


def start(root, identity, logical_id, *, retry_of=None, reason=None):
    """Reserve immutable execution identity before a simulated dispatch."""
    root = Path(root)
    manifest = validate(root, identity)
    if logical_id not in {t['logical_id'] for t in manifest['trials']}:
        raise ValueError('unknown trial')
    previous = [e for e in executions(root) if e['logical_id'] == logical_id]
    if previous:
        predecessors = {e['predecessor'] for e in previous if e['predecessor']}
        leaves = [e for e in previous if e['execution_id'] not in predecessors]
        if len(leaves) != 1:
            raise ValueError('invalid retry lineage')
        last = leaves[0]
        if retry_of != last['execution_id'] or last['status'] not in ('incomplete', 'error') or not reason:
            raise ValueError('explicit retry of latest incomplete/error with reason required')
    elif retry_of is not None:
        raise ValueError('retry predecessor missing')
    eid = uuid4().hex
    folder = root / 'executions' / eid
    folder.mkdir(mode=0o700)
    parent_fd = os.open(folder.parent, os.O_RDONLY)
    try:
        os.fsync(parent_fd)
    finally:
        os.close(parent_fd)
    # Unique fresh native job name: never resume a prior Harbor job directory.
    record = {'execution_id': eid, 'logical_id': logical_id,
              'native_job_name': ('planar-control-' if manifest['kind'] == 'injected-control-only' else 'planar-t044-') + eid,
              'predecessor': retry_of, 'retry_reason': reason,
              'stimulus': 'injected' if manifest['kind'] == 'injected-control-only' else None, 'reserved_at': datetime.now(timezone.utc).isoformat()}
    create(folder / 'start.json', record)
    return record


def finish(root, execution_id, status, reason):
    if status not in ('pass', 'behavioral_failure', 'incomplete', 'error') or not reason:
        raise ValueError('explicit status and reason required')
    folder = Path(root) / 'executions' / execution_id
    if not (folder / 'start.json').is_file():
        raise ValueError('unknown execution')
    create(folder / 'terminal.json', {'status': status, 'reason': reason, 'recorded_at': datetime.now(timezone.utc).isoformat()})


def report(root, identity):
    root = Path(root)
    m = validate(root, identity)
    rows = executions(root)
    groups = []
    for variant in m['variants']:
        trials = [t for t in m['trials'] if t['variant'] == variant]
        counts = dict.fromkeys(('planned', 'reserved', 'started', 'pass', 'behavioral_failure', 'incomplete', 'error', 'not_started'), 0)
        counts['planned'] = len(trials)
        for trial in trials:
            # Primary comparison is always the first execution, never best retry.
            attempts = [e for e in rows if e['logical_id'] == trial['logical_id']]
            if not attempts:
                counts['not_started'] += 1
                continue
            primary = next((e for e in attempts if e['predecessor'] is None), None)
            if primary is None:
                raise ValueError('missing primary execution')
            counts['reserved'] += 1
            if primary['native'] or primary['live_result'] or (primary['dispatch'] and (Path(primary['dispatch']['evidence']) / 'native-trial-start.json').is_file()):
                counts['started'] += 1
            counts[primary['status']] += 1
        completed = counts['pass'] + counts['behavioral_failure']
        groups.append({'variant': variant, **counts, 'evaluated': completed, 'pass_rate': counts['pass'] / completed if completed else None,
                       'coverage': completed / len(trials) if trials else None})
    pending = [e for e in rows if e['account_stop'] and not e['account_stop_resolution']]
    trials = []
    for trial in m['trials']:
        attempts = [e for e in rows if e['logical_id'] == trial['logical_id']]
        primary = next((e for e in attempts if e['predecessor'] is None), None)
        if attempts and primary is None:
            raise ValueError('missing primary execution')
        reason = primary['reason'] if primary else ('not started: account stop unresolved' if pending else 'not started: no execution reserved')
        trials.append({**trial, 'reason': reason})
    return {'identity': identity, 'kind': m['kind'], 'created_at': m.get('created_at'), 'deadline_at': m['deadline_at'], 'caps': m['caps'], 'variant_efforts': m.get('variant_efforts'), 'configured_agents': m['config']['agents'], 'pins': m['pins'], 'resolved_pins': m['resolved_pins'], 'pin_provenance': m['pin_provenance'], 'factors': m['factors'], 'trials': trials, 'executions': rows, 'groups': groups}


def markdown(result):
    label = 'Injected experiment controls' if result['kind'] == 'injected-control-only' else 'Codex experiment'
    lines = ['# ' + label, '',
             '| Variant | Planned | Reserved | Started | Evaluated | Pass | Failure | Incomplete | Error | Not started | Pass rate | Coverage |',
             '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for g in result['groups']:
        rate = 'undefined' if g['pass_rate'] is None else f"{100*g['pass_rate']:.1f}%"
        coverage = 'undefined' if g['coverage'] is None else f"{100*g['coverage']:.1f}%"
        lines.append(f"| {g['variant']} | {g['planned']} | {g['reserved']} | {g['started']} | {g['evaluated']} | {g['pass']} | {g['behavioral_failure']} | {g['incomplete']} | {g['error']} | {g['not_started']} | {rate} | {coverage} |")
    lines.extend(['', '## Configuration and provenance', '', '```json', json.dumps({k: result.get(k) for k in ('identity', 'created_at', 'deadline_at', 'caps', 'variant_efforts', 'configured_agents', 'pins', 'resolved_pins', 'pin_provenance', 'factors')}, indent=2), '```', '', '## Planned trials', ''])
    for trial in result['trials']:
        lines.append(f"- {trial['logical_id']}: {trial['reason']}")
    lines.extend(['', '## Executions', ''])
    for e in result['executions']:
        native = e['live_result']['native'] if e['live_result'] else None
        links = ''
        if native:
            trial_dir = Path(native['job_dir']) / native['trial_name']
            links = f"; [native result](<{trial_dir / 'result.json'}>); [verifier artifacts](<{trial_dir / 'verifier'}>)"
        lines.append(f"- {e['logical_id']}: {e['status']} — {e['reason']}; [execution evidence](<{e['evidence']}>); predecessor: {e['predecessor'] or 'none'}; usage: {e['usage'] if e['usage'] is not None else 'unavailable'}; timing: {e['timing'] if e['timing'] is not None else 'unavailable'}; observed: {e['observed'] if e['observed'] else 'unavailable'}{links}")
    lines.append('')
    return '\n'.join(lines)




def native_completion_consistent(job, trial):
    """Accept only one coherent completed native execution; never infer a grade."""
    stats = job.stats
    exceptions = {e.exception_type for e in trial.recorded_exceptions()}
    if (job.n_total_trials != 1 or stats.n_completed_trials != 1
        or stats.n_pending_trials != 0 or stats.n_running_trials != 0 or stats.n_retries != 0
        or stats.n_errored_trials != int(bool(exceptions))
        or stats.n_cancelled_trials != int('CancelledError' in exceptions)
        or job.started_at is None or job.finished_at is None
        or trial.started_at is None or trial.finished_at is None):
        return False
    try:
        # Harbor 0.26 writes naive job timestamps in local time and UTC-aware
        # trial timestamps. Normalize both to UTC before comparing records.
        job_start, trial_start, trial_finish, job_finish = (
            value.astimezone(timezone.utc) for value in
            (job.started_at, trial.started_at, trial.finished_at, job.finished_at)
        )
        return job_start <= trial_start <= trial_finish <= job_finish
    except (TypeError, ValueError, OverflowError, OSError):
        return False

def reconcile_nop(root):
    """Reconcile complete no-model native evidence without inferring a pass."""
    from harbor.models.job.result import JobResult
    from harbor.models.trial.result import TrialResult
    for folder in (Path(root).resolve(strict=True) / 'executions').iterdir():
        if (folder / 'terminal.json').exists():
            continue
        try:
            reservation = read(folder / 'start.json')
            native = read(folder / 'native-start.json')
            complete = read(folder / 'native-complete.json')
            job_dir = folder / reservation['native_job_name']
            name = native['trial_name']
            if Path(name).name != name or Path(native['raw_job']) != job_dir:
                continue
            job = JobResult.model_validate_json((job_dir / 'result.json').read_text())
            trial = TrialResult.model_validate_json((job_dir / name / 'result.json').read_text())
            if (str(job.id) != native['job_id'] or str(job.id) != complete['job_id']
                or str(trial.id) != native['trial_id'] or trial.trial_name != name
                or not native_completion_consistent(job, trial) or trial.verifier_result is not None):
                continue
            finish(root, reservation['execution_id'], 'incomplete', 'reconciled nop native results: verifier disabled')
        except (OSError, KeyError, ValueError):
            continue


def resolve_nop_stop(root, identity, execution_id, reason):
    import fcntl
    root = Path(root).resolve(strict=True)
    if not reason or not isinstance(reason, str):
        raise ValueError('explicit resolution reason required')
    with (root / 'controller.lock').open('a+') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if validate(root, identity)['kind'] != 'injected-control-only':
            raise ValueError('not a nop control')
        folder = root / 'executions' / execution_id
        event = folder / 'injected-event.json'
        usage_stop = event.is_file() and read(event).get('event') == 'usage_limit'
        ambiguous_stop = (folder / 'start.json').is_file() and not (folder / 'terminal.json').exists() and (folder / 'account-stop.json').is_file()
        if not usage_stop and not ambiguous_stop:
            raise ValueError('no unresolved nop stop')
        create(folder / 'account-stop-resolved.json', {'reason': reason})

def run_nop(root, identity, *, max_new=None, retry_of=None, retry_reason=None, injected_events=None):
    """Serial no-model jobs only; continuation never resumes a native job."""
    import fcntl
    import sys
    import time
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'adapters'))
    from supervise import supervise

    root = Path(root).resolve(strict=True)
    m = validate(root, identity)
    if m['kind'] != 'injected-control-only':
        raise ValueError('nop runner cannot dispatch Codex experiments')
    if max_new is not None and (not isinstance(max_new, int) or max_new < 1):
        raise ValueError('max_new must be positive')
    lock = open(root / 'controller.lock', 'a+')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        reconcile_nop(root)
        for folder in (root / 'executions').iterdir():
            event = folder / 'injected-event.json'
            resolved = (folder / 'account-stop-resolved.json').exists()
            if event.is_file() and read(event).get('event') == 'usage_limit' and not resolved:
                raise ValueError('injected usage stop requires explicit resolution')
            if not (folder / 'terminal.json').exists() and not resolved:
                if not (folder / 'account-stop.json').exists():
                    create(folder / 'account-stop.json', {'reason': 'ambiguous native evidence', 'source': 'nop-control'})
                raise ValueError('ambiguous nop execution requires explicit resolution')
        # Absolute deadline includes pauses; continuation cannot reset the cap.
        remaining_wall = (datetime.fromisoformat(m['deadline_at']) - datetime.now(timezone.utc)).total_seconds()
        deadline = time.monotonic() + max(0, remaining_wall)
        launched = 0
        previous = executions(root)
        selected = next((e for e in previous if e['execution_id'] == retry_of), None) if retry_of else None
        if retry_of and (selected is None or not retry_reason):
            raise ValueError('retry requires a known predecessor and reason')
        for trial in m['trials']:
            if retry_of and trial['logical_id'] != selected['logical_id']:
                continue
            if not retry_of and any(e['logical_id'] == trial['logical_id'] for e in executions(root)):
                continue
            if len(executions(root)) >= m['caps']['trials']:
                break
            if max_new is not None and launched >= max_new:
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            reservation = start(root, identity, trial['logical_id'], retry_of=retry_of, reason=retry_reason)
            folder = root / 'executions' / reservation['execution_id']
            env = {'PATH': os.environ.get('PATH', ''), 'HOME': str(root), 'HARBOR_TELEMETRY': '0'}
            status = supervise([sys.executable, str(Path(__file__).with_name('native_control.py')), str(folder)],
                deadline=min(remaining, m['caps']['trial_seconds']), grace=2, cleanup=lambda: None, env=env)
            if status.get('status') != 'completed' or status.get('return_code') != 0:
                kind = 'incomplete' if status.get('status') == 'interrupted' else 'error'
                finish(root, reservation['execution_id'], kind, 'native control interrupted or infrastructure failure')
                break
            if not (folder / 'native-start.json').is_file() or not (folder / 'native-complete.json').is_file():
                finish(root, reservation['execution_id'], 'incomplete', 'native result or identity missing')
                break
            event = (injected_events or {}).get(trial['logical_id'])
            classifications = {'usage_limit': ('incomplete', 'injected usage limit'), 'timeout': ('incomplete', 'injected timeout'), 'authentication': ('error', 'injected authentication failure'), 'missing_plugin': ('error', 'injected missing plugin'), 'verifier_crash': ('error', 'injected verifier crash')}
            if event is not None:
                if event not in classifications:
                    raise ValueError('unknown injected event')
                create(folder / 'injected-event.json', {'event': event, 'source': 'test-only'})
                status, reason = classifications[event]
                if event == 'usage_limit':
                    create(folder / 'account-stop.json', {'reason': reason, 'source': 'injected'})
            else:
                status, reason = 'incomplete', 'nop control: verifier disabled; no behavior grade'
            finish(root, reservation['execution_id'], status, reason)
            launched += 1
            if event == 'usage_limit':
                break
            if retry_of:
                break
        return launched
    finally:
        lock.close()
