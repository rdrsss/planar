"""One explicitly gated Codex execution from a frozen experiment plan.

Each call needs its own existing one-use launcher gate; this module never
creates approvals, changes authentication routes, or resumes a native job.
"""
import fcntl
import hashlib
import json
from contextlib import contextmanager
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'adapters'))
import launch
from harbor.models.trial.result import TrialResult
from harbor.models.job.result import JobResult
from harbor.models.job.config import JobConfig
import controls
import native_status



@contextmanager
def admission_scope():
    """One conservative account scope per configured output root."""
    root = launch.reviews_root().resolve()
    root.mkdir(parents=True, exist_ok=True)
    scope = root / 'planar-account-admission-v1'
    if scope.is_symlink():
        raise ValueError('admission scope cannot be a symlink')
    scope.mkdir(mode=0o700, exist_ok=True)
    records = scope / 'records'
    if records.is_symlink():
        raise ValueError('admission records cannot be a symlink')
    records.mkdir(mode=0o700, exist_ok=True)
    with (scope / 'lock').open('a+') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield records


def shared_pending(records):
    def released(start):
        closed=start.with_name(start.name.replace('.start.json','.closed.json'))
        resolved=start.with_name(start.name.replace('.start.json','.resolved.json'))
        try:
            data=json.loads(start.read_text())
            if data.get('case') != 'retained-a-fresh-b': return closed.exists() or resolved.exists()
            if closed.exists() or closed.is_symlink() or resolved.is_symlink() or not resolved.is_file(): return False
            import retained_b_release_schema
            release=json.loads(resolved.read_text(),object_pairs_hook=retained_b_release_schema._unique)
            early_fields={'type','execution_id','job_id','trial_id','observed_status','observed_reason','observed_reward',
                          'gate_sha256','source_packet_sha256','case_sha256','candidate_sha256','configuration_sha256',
                          'historical_implementation_sha256','resolver_commit','resolver_implementation_sha256',
                          'audit_sha256','release_sha256','release_path','historical_commit','wrapper_audit_sha256','trace_sha256','verifier_status_sha256','reward_sha256','census_sha256'}
            if release.get('type')=='aborted-before-work':
                gate=data.get('gate_authority',{})
                review=Path(release['release_path']); project_reviews=(launch.PROJECT/'reviews').resolve(strict=True)
                if review.is_symlink() or not review.is_file() or not review.resolve().is_relative_to(project_reviews): return False
                review_bytes=review.read_bytes()
                if hashlib.sha256(review_bytes).hexdigest()!=release['release_sha256']: return False
                reviewed=retained_b_release_schema.parse(review_bytes)
                overlap=set(release)&set(reviewed)
                if any(type(release[k]) is not type(reviewed[k]) or release[k]!=reviewed[k] for k in overlap): return False
                census=resolved.parent/(data['execution_id']+'.census-'+release['census_sha256']+'.json')
                if census.is_symlink() or not census.is_file() or hashlib.sha256(census.read_bytes().rstrip(b'\n')).hexdigest()!=release['census_sha256']: return False
                return (set(release)==early_fields and release['execution_id']==data['execution_id']=='planar-t044-aeaec469c5c549ee96f3ce7729dc4c2e'
                        and release['job_id']=='fdc75ee2-c657-453d-ab8a-0b86879b5e36' and release['trial_id']=='89273082-831e-4cfd-812e-307db6df2413'
                        and release['historical_commit']=='bd3fecfe1c4786125973fc800150173f52be1f53' and release['wrapper_audit_sha256']=='fe6f155376212c9d77d01ecd8bb539ac99b479a5169ee754ff7282ec6c559572'
                        and release['observed_status']=='behavioral_failure' and release['observed_reason']=='final record field mismatch' and type(release['observed_reward']) is int and release['observed_reward']==0
                        and release['gate_sha256']==data['gate_sha256']=='8a7d024b4f8ec14983f700e352deeddfeeb9209fb28a4d9545fd6336132afe86' and release['source_packet_sha256']==gate.get('source_packet_sha256')
                        and release['audit_sha256']=='1b6f7e187c154fce391d7eccd341293fc19e00f6cfbbc949255838c9a5413578'
                        and release['trace_sha256']=='7ebac5fa973365f637365c54fdb675852623304767f45c6c9c895049a4b1e0cc'
                        and release['verifier_status_sha256']=='d638a8043ee92815310aecbeb69060bc3a5464e08ff76f12b40252cfe0ae946e'
                        and release['reward_sha256']=='9a271f2a916b0b6ee6cecb2426f0b3206ef074578be55d9bc94f6f3fe3ab86aa'
                        and release['case_sha256']==gate.get('case_sha256') and release['candidate_sha256']==gate.get('candidate_sha256')
                        and release['configuration_sha256']==gate.get('configuration_sha256') and release['historical_implementation_sha256']==gate.get('implementation_sha256')
                        and isinstance(release['resolver_commit'],str) and len(release['resolver_commit'])==40 and all(c in '0123456789abcdef' for c in release['resolver_commit'])
                        and all(isinstance(release[k],str) and len(release[k])==64 and all(c in '0123456789abcdef' for c in release[k]) for k in ('gate_sha256','source_packet_sha256','case_sha256','candidate_sha256','configuration_sha256','historical_implementation_sha256','resolver_implementation_sha256','audit_sha256','release_sha256','trace_sha256','verifier_status_sha256','reward_sha256','census_sha256')))
            fields={'execution_id','behavior','audit_sha256','gate_sha256','source_packet_sha256','verifier_status_sha256','reward_sha256'}
            return (set(release)==fields and release['execution_id']==data['execution_id'] and release['behavior']=='incomplete'
                    and release['gate_sha256']==data['gate_sha256'] and release['source_packet_sha256']==data['gate_authority']['source_packet_sha256']
                    and all(isinstance(release[k],str) and len(release[k])==64 and all(c in '0123456789abcdef' for c in release[k]) for k in ('audit_sha256','gate_sha256','source_packet_sha256','verifier_status_sha256','reward_sha256')))
        except (OSError,ValueError,TypeError,KeyError,AttributeError): return False
    return [p for p in sorted(records.glob('*.start.json')) if not released(p)]


def preflight_one(root, identity, logical_id):
    root = Path(root).resolve(strict=True)
    manifest = controls.validate(root, identity)
    if manifest['kind'] != 'codex-experiment':
        raise ValueError('a Codex experiment definition is required')
    selected = next((t for t in manifest['trials'] if t['logical_id'] == logical_id), None)
    if selected is None:
        raise ValueError('unknown trial')
    deadline = min(manifest['caps']['trial_seconds'], 1500)
    plan = launch.plan(manifest['pins']['case_name'], candidate_root=root / 'candidate-snapshot',
        case_root=root / 'case-snapshot', trial_deadline=deadline,
        effort=manifest['variant_efforts'][selected['variant']])
    budgets = plan['prerequisites']['budgets_seconds']
    if budgets['agent'] > manifest['caps']['session_seconds'] or budgets['verifier'] > manifest['caps']['verifier_seconds']:
        raise ValueError('case session or verifier deadline exceeds experiment cap')
    plan['admission_scope'] = str(launch.reviews_root().resolve() / 'planar-account-admission-v1')
    plan['experiment_identity'] = identity
    plan['logical_id'] = logical_id
    plan['experiment_implementation_sha256'] = controls.tree_digest(Path(__file__).resolve().parent)
    return plan



def validate_gate(root, identity, logical_id, review_path):
    """Validate an existing exact one-use gate without consuming it."""
    preflight = preflight_one(root, identity, logical_id)
    review = Path(review_path).resolve(strict=True)
    if not review.is_relative_to(launch.reviews_root().resolve()):
        raise ValueError('review must reside under the reviews root')
    gate = controls.read(review)
    required = {'verdict': 'approve', 'purpose': 'one-diagnostic-attempt', 'max_attempts': 1}
    required.update({k: preflight[k] for k in ('implementation_sha256', 'candidate_sha256', 'configuration_sha256', 'case', 'case_sha256', 'image_id', 'experiment_identity', 'logical_id', 'experiment_implementation_sha256', 'admission_scope')})
    if any(gate.get(k) != v for k, v in required.items()):
        raise ValueError('experiment review missing, stale or not approved')
    gate_digest = hashlib.sha256(json.dumps(gate, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    if (launch.reviews_root() / 'harbor-t044-consumed' / gate_digest).exists():
        raise ValueError('experiment review already consumed')
    return preflight

def reconcile(folder):
    """Reconstruct a missing summary only from native result and cleanup proof."""
    folder = Path(folder)
    if (folder / 'terminal.json').exists():
        return None
    try:
        start = controls.read(folder / 'start.json')
        dispatch = controls.read(folder / 'dispatch.json')
        evidence = Path(dispatch['evidence'])
        launcher_id = dispatch['launcher_id']
        if launcher_id != 'planar-t044-' + start['execution_id'] or evidence.name != launcher_id:
            raise ValueError('launcher identity mismatch')
        if (evidence / 'recovery-required').exists():
            raise ValueError('cleanup is unconfirmed')
        terminal = controls.read(evidence / 'launcher-terminal.json')
        watchdog = controls.read(evidence / 'watchdog-terminal.json')
        if terminal.get('status') != 'completed' or watchdog.get('status') != 'completed' or watchdog.get('return_code') != 0:
            raise ValueError('launcher completion or cleanup is unconfirmed')
        native = controls.read(evidence / 'native-trial-start.json')
        job_dir = evidence / launcher_id
        if Path(native['job_dir']) != job_dir or Path(native['trial_name']).name != native['trial_name']:
            raise ValueError('native evidence path mismatch')
        trial = TrialResult.model_validate_json((job_dir / native['trial_name'] / 'result.json').read_text())
        job = JobResult.model_validate_json((job_dir / 'result.json').read_text())
        config = JobConfig.model_validate_json((job_dir / 'config.json').read_text())
        submitted = JobConfig.model_validate_json((evidence / 'job-config.json').read_text())
        if (str(trial.id) != native['trial_id'] or str(trial.config.job_id) != native['job_id']
            or str(job.id) != native['job_id'] or trial.trial_name != native['trial_name']
            or trial.config.trial_name != native['trial_name'] or not controls.native_completion_consistent(job, trial)
            or config != submitted
            or len(config.agents) != 1 or len(config.tasks) != 1
            or trial.config.agent != config.agents[0] or trial.config.task != config.tasks[0]):
            raise ValueError('native identity or result contradiction')
        records = launch.reviews_root() / 'planar-account-admission-v1' / 'records'
        for entry in records.glob('*.start.json'):
            other_id = controls.read(entry)['execution_id']
            if other_id == start['execution_id']:
                continue
            other = launch.jobs_root() / ('planar-t044-' + other_id) / 'native-trial-start.json'
            if other.is_file():
                other_native = controls.read(other)
                if other_native.get('trial_id') == native['trial_id'] or other_native.get('job_id') == native['job_id']:
                    raise ValueError('duplicate native identity')
        outcome = native_status.assess_verified(trial, job_dir / native['trial_name'] / 'verifier')
        tokens = trial.compute_token_cost_totals()
        result = {'native': native, 'classification': outcome,
                  'timing': {'started_at': str(trial.started_at) if trial.started_at else None,
                             'finished_at': str(trial.finished_at)},
                  'usage': {'input_tokens': tokens[0], 'cache_tokens': tokens[1],
                            'output_tokens': tokens[2], 'cost_usd': tokens[3],
                            'provenance': 'native TrialResult; cost is not subscription spend'}}
        record = folder / 'native-result.json'
        if record.exists():
            if controls.read(record) != result:
                raise ValueError('conflicting retained result')
        else:
            controls.create(record, result)
        if outcome['stop_account'] and not (folder / 'account-stop.json').exists():
            controls.create(folder / 'account-stop.json', {'reason': outcome['reason']})
        controls.finish(folder.parent.parent, start['execution_id'], outcome['status'], outcome['reason'])
        return outcome
    except (OSError, ValueError, KeyError):
        return None


def pending_account_stops(root):
    pending = []
    for folder in (Path(root) / 'executions').iterdir():
        result_path = folder / 'native-result.json'
        classified_stop = result_path.is_file() and controls.read(result_path)['classification']['stop_account']
        if ((folder / 'account-stop.json').is_file() or classified_stop) and not (folder / 'account-stop-resolved.json').exists():
            pending.append(folder.name)
    return pending


def resolve_account_stop(root, identity, execution_id, reason):
    root = Path(root).resolve(strict=True)
    if not reason or not isinstance(reason, str):
        raise ValueError('explicit resolution reason required')
    with admission_scope() as records, (root / 'controller.lock').open('a+') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        controls.validate(root, identity)
        shared = records / (execution_id + '.start.json')
        if not shared.is_file() or controls.read(shared).get('experiment') != str(root):
            raise ValueError('no matching scoped admission record')
        shared_resolution = records / (execution_id + '.resolved.json')
        if (records / (execution_id + '.closed.json')).exists() or shared_resolution.exists():
            raise ValueError('no unresolved account stop for execution')
        folder = root / 'executions' / execution_id
        if execution_id not in pending_account_stops(root) and not (folder / 'account-stop-resolved.json').exists():
            controls.create(folder / 'account-stop.json', {'reason': 'shared admission not closed'})
        local_resolution = folder / 'account-stop-resolved.json'
        if local_resolution.exists():
            if controls.read(local_resolution) != {'reason': reason}:
                raise ValueError('conflicting resolution reason')
        else:
            controls.create(local_resolution, {'reason': reason})
        controls.create(shared_resolution, {'reason': reason})

def execute_one(root, identity, logical_id, auth_path, review_path, *, retry_of=None, retry_reason=None):
    root = Path(root).resolve(strict=True)
    with admission_scope() as records, (root / 'controller.lock').open('a+') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        manifest = controls.validate(root, identity)
        if manifest['kind'] != 'codex-experiment':
            raise ValueError('a Codex experiment definition is required')
        for folder in (root / 'executions').iterdir():
            if not (folder / 'terminal.json').exists():
                if reconcile(folder) is None and not (folder / 'account-stop.json').exists():
                    controls.create(folder / 'account-stop.json', {'reason': 'ambiguous interrupted execution'})
        if shared_pending(records) or pending_account_stops(root):
            raise ValueError('account stop requires explicit resolution before dispatch')
        if len(controls.executions(root)) >= manifest['caps']['trials']:
            raise ValueError('total execution cap reached')
        remaining = (datetime.fromisoformat(manifest['deadline_at']) - datetime.now(timezone.utc)).total_seconds()
        deadline = min(manifest['caps']['trial_seconds'], 1500)
        if remaining < deadline:
            raise ValueError('experiment deadline expired')
        selected = next((t for t in manifest['trials'] if t['logical_id'] == logical_id), None)
        if selected is None:
            raise ValueError('unknown trial')
        case_name = manifest['pins']['case_name']
        effort = manifest['variant_efforts'][selected['variant']]
        candidate = root / 'candidate-snapshot'
        case = root / 'case-snapshot'
        # Preflight before reserving; execute rechecks the gate, auth, image and cleanup.
        validate_gate(root, identity, logical_id, review_path)
        execution = controls.start(root, identity, logical_id, retry_of=retry_of, reason=retry_reason)
        eid = execution['execution_id']
        launcher_id = 'planar-t044-' + eid
        folder = root / 'executions' / eid
        evidence = launch.jobs_root() / launcher_id
        controls.create(records / (eid + '.start.json'), {'experiment': str(root), 'execution_id': eid})
        controls.create(folder / 'dispatch.json', {'launcher_id': launcher_id, 'evidence': str(evidence),
            'case': case_name, 'effort': effort, 'trial_deadline': deadline, 'admission_scope': str(records.parent)})
        try:
            launch.execute(Path(auth_path), Path(review_path), case_name, candidate_root=candidate,
                           case_root=case, trial_deadline=deadline, execution_id=launcher_id, effort=effort,
                           absolute_deadline=manifest['deadline_at'], emit=False)
        except Exception as exc:
            controls.create(folder / 'account-stop.json', {'reason': 'guarded launcher failure'})
            controls.finish(root, eid, 'error', 'guarded launcher failed: ' + type(exc).__name__)
            return {'execution_id': eid, 'status': 'error', 'stop_account': True}
        outcome = reconcile(folder)
        if outcome is None:
            controls.create(folder / 'account-stop.json', {'reason': 'native result or cleanup unavailable'})
            controls.finish(root, eid, 'incomplete', 'native result or cleanup evidence unavailable')
            return {'execution_id': eid, 'status': 'incomplete', 'stop_account': True}
        if not outcome['stop_account']:
            controls.create(records / (eid + '.closed.json'), {'status': outcome['status']})
        return {'execution_id': eid, **outcome}


def execute_batch(root, identity, auth_path, gates):
    """Execute only unreserved logical trials, each with its own existing gate."""
    root = Path(root).resolve(strict=True)
    manifest = controls.validate(root, identity)
    if manifest['kind'] != 'codex-experiment' or not isinstance(gates, dict):
        raise ValueError('Codex experiment and logical-ID gate map required')
    existing = {e['logical_id'] for e in controls.executions(root)}
    pending = [t['logical_id'] for t in manifest['trials'] if t['logical_id'] not in existing]
    if set(gates) != set(pending) or any(not isinstance(v, str) or not v for v in gates.values()):
        raise ValueError('exactly one gate for each unreserved trial is required')
    if len(controls.executions(root)) + len(pending) > manifest['caps']['trials']:
        raise ValueError('batch exceeds total execution cap')
    gate_paths = [Path(gates[logical]).resolve(strict=True) for logical in pending]
    if len(set(gate_paths)) != len(gate_paths):
        raise ValueError('each trial requires a distinct gate')
    for logical in pending:
        validate_gate(root, identity, logical, gates[logical])
    results = []
    for logical in pending:
        result = execute_one(root, identity, logical, auth_path, gates[logical])
        results.append({'logical_id': logical, **result})
        if result.get('stop_account'):
            break
    return {'executions': results, 'not_dispatched': pending[len(results):]}
