"""Read-only schema-2 reservation inventory, not behavioral reconciliation."""
from pathlib import Path
import schema2
import per_variant

RESERVATION_FIELDS = frozenset(('schema_version', 'state', 'reservation_id', 'execution_id', 'experiment_identity', 'logical_id', 'variant_id', 'repetition', 'predecessor_execution_id', 'retry_reason_sha256', *schema2.IDENTITIES, 'configuration_sha256', 'implementation_sha256', 'image_id', 'verifier_image_id', 'host_version', 'account_scope_id', 'canonical_output_root', 'expires_at'))

def check_optional_linkage(record, reservation):
    if not isinstance(record, dict): raise schema2.Invalid('invalid execution record')
    for key in RESERVATION_FIELDS - {'schema_version', 'state', 'expires_at'}:
        if key in record and record[key] != reservation[key]:
            raise schema2.Invalid(f'contradictory execution record: {key}')


def report(root, identity):
    root = Path(root)
    manifest = schema2.validate(root, identity)
    trials = {t['logical_id']: {**t, 'attempts': []} for t in manifest['trials']}
    for path in sorted((root/'reservations').iterdir()):
        if path.suffix != '.json' or path.is_symlink() or not path.is_file():
            raise schema2.Invalid('unsafe reservation entry')
        r = schema2.read_record(path)
        eid = r.get('reservation_id')
        if not isinstance(eid, str) or len(eid) != 32 or any(c not in '0123456789abcdef' for c in eid) or path.name != eid+'.json' or r.get('execution_id') != eid or r.get('experiment_identity') != identity:
            raise schema2.Invalid('invalid reservation identity')
        trial = trials.get(r.get('logical_id'))
        if trial is None or any(r.get(k) != trial[k] for k in ('variant_id','repetition')):
            raise schema2.Invalid('reservation trial mismatch')
        variant = next(v for v in manifest['variants'] if v['variant_id'] == trial['variant_id'])
        expected = {**{k:variant[k] for k in (*schema2.IDENTITIES, 'configuration_sha256', 'image_id', 'verifier_image_id', 'host_version')},
            'implementation_sha256':manifest['implementation_sha256'], 'account_scope_id':manifest['configuration']['account_scope_id'],
            'canonical_output_root':manifest['configuration']['canonical_output_root'], 'schema_version':2, 'state':'planned'}
        if set(r) != RESERVATION_FIELDS or type(r['schema_version']) is not int or type(r['repetition']) is not int or any(r[k] != v for k,v in expected.items()):
            raise schema2.Invalid(f'reservation contradicts frozen experiment: {path.name}')
        predecessor, reason = r['predecessor_execution_id'], r['retry_reason_sha256']
        if predecessor is None:
            if reason is not None: raise schema2.Invalid('primary retry reason')
        elif not isinstance(predecessor,str) or len(predecessor) != 32 or any(c not in '0123456789abcdef' for c in predecessor) or not isinstance(reason,str) or not per_variant.HEX.fullmatch(reason):
            raise schema2.Invalid('invalid retry identity')
        if schema2.utc(r['expires_at']) > schema2.utc(manifest['deadline_at']): raise schema2.Invalid('reservation expiry exceeds deadline')
        folder = root/'executions'/eid
        attempt = {'reservation_id':eid, 'predecessor_execution_id':r.get('predecessor_execution_id'), 'state':'planned', 'native_start_record_present_unverified':False, 'recorded_terminal':None}
        if folder.is_symlink(): raise schema2.Invalid('unsafe execution entry')
        if folder.exists():
            if not folder.is_dir(): raise schema2.Invalid('unsafe execution entry')
            start = schema2.read_record(folder/'start.json')
            if start.get('execution_id') != eid or start.get('logical_id') != trial['logical_id']:
                raise schema2.Invalid('execution start mismatch')
            check_optional_linkage(start, r)
            attempt['state'] = 'execution_record_present_unverified'
            native = folder/'native-start.json'
            if native.exists() or native.is_symlink():
                native_record = schema2.read_record(native)
                check_optional_linkage(native_record, r)
                attempt['native_start_record_present_unverified'] = True
            terminal = folder/'terminal.json'
            if terminal.exists() or terminal.is_symlink():
                record = schema2.read_record(terminal)
                check_optional_linkage(record, r)
                if record.get('status') not in ('pass','behavioral_failure','incomplete','error'):
                    raise schema2.Invalid('invalid terminal status')
                attempt['recorded_terminal'] = record
        trial['attempts'].append(attempt)
    # An execution without a reservation is not attributable to this plan.
    for folder in (root/'executions').iterdir():
        if folder.is_symlink() or not folder.is_dir() or not (root/'reservations'/(folder.name+'.json')).is_file():
            raise schema2.Invalid('foreign execution')
    groups = []
    for variant in manifest['variants']:
        rows = [t for t in trials.values() if t['variant_id'] == variant['variant_id']]
        primary = []
        for row in rows:
            attempts = row['attempts']
            roots = [a for a in attempts if a['predecessor_execution_id'] is None]
            if attempts and len(roots) != 1: raise schema2.Invalid('ambiguous primary reservation')
            by_id = {a['reservation_id']:a for a in attempts}
            visited = set()
            current = roots[0] if roots else None
            while current:
                eid = current['reservation_id']
                if eid in visited: raise schema2.Invalid('cyclic retry lineage')
                visited.add(eid)
                successors = [a for a in attempts if a['predecessor_execution_id'] == eid]
                if len(successors) > 1: raise schema2.Invalid('forked retry lineage')
                current = successors[0] if successors else None
            if len(visited) != len(by_id): raise schema2.Invalid('disconnected retry lineage')
            primary.append(roots[0] if roots else None)
        all_attempts = [a for row in rows for a in row['attempts']]
        groups.append({'variant_id':variant['variant_id'], 'planned_logical_trials':len(rows),
            'primary_reserved':sum(x is not None for x in primary),
            'primary_not_started':sum(x is None or x['state'] == 'planned' for x in primary),
            'primary_execution_record_present_unverified':sum(bool(x and x['state'] != 'planned') for x in primary),
            'primary_native_start_record_present_unverified':sum(bool(x and x['native_start_record_present_unverified']) for x in primary),
            'primary_terminal_record_present_unverified':sum(bool(x and x['recorded_terminal']) for x in primary),
            'all_attempts_reserved':len(all_attempts),
            'all_attempts_planned':sum(x['state'] == 'planned' for x in all_attempts),
            'all_attempts_execution_record_present_unverified':sum(x['state'] != 'planned' for x in all_attempts),
            'all_attempts_native_start_record_present_unverified':sum(x['native_start_record_present_unverified'] for x in all_attempts),
            'all_attempts_terminal_record_present_unverified':sum(bool(x['recorded_terminal']) for x in all_attempts),
            'retry_attempts':sum(max(0,len(row['attempts'])-1) for row in rows)})
    return {'schema_version':2, 'experiment_identity':identity,
        'assessment':'unreconciled: reservation and recorded execution inventory only; no behavioral verdict inferred',
        'groups':groups, 'trials':list(trials.values())}
