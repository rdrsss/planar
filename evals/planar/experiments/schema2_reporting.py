"""Read-only schema-2 reservation inventory, not behavioral reconciliation."""
from pathlib import Path
import schema2
import per_variant
import json

RESERVATION_FIELDS = frozenset(('schema_version', 'state', 'reservation_id', 'execution_id', 'experiment_identity', 'logical_id', 'variant_id', 'repetition', 'predecessor_execution_id', 'retry_reason_sha256', *schema2.IDENTITIES, 'configuration_sha256', 'implementation_sha256', 'image_id', 'verifier_image_id', 'host_version', 'account_scope_id', 'canonical_output_root', 'expires_at'))

def check_optional_linkage(record, reservation):
    if not isinstance(record, dict): raise schema2.Invalid('invalid execution record')
    for key in RESERVATION_FIELDS - {'schema_version', 'state', 'expires_at'}:
        if key in record and record[key] != reservation[key]:
            raise schema2.Invalid(f'contradictory execution record: {key}')


def report(root, identity):
    root = Path(root)
    manifest = schema2.validate(root, identity)
    runnable = manifest.get('runnable_contract') is not None
    if runnable:
        import schema2_runnable
        if manifest['runnable_contract'] not in (schema2_runnable.CONTRACT,schema2_runnable.LEGACY_CONTRACT): raise schema2.Invalid('unknown runnable contract')
        schema2_runnable.closed(manifest,schema2_runnable.MANIFEST_FIELDS)
        if manifest['runnable_contract'] == schema2_runnable.CONTRACT: schema2_runnable.validate_context(manifest)
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
        allowed = RESERVATION_FIELDS | ({'runnable_contract','concrete_binding'} if runnable else set())
        if runnable and manifest['runnable_contract']==schema2_runnable.CONTRACT:
            allowed |= schema2_runnable.CONTEXT_FIELDS
            expected.update({'selection_context_sha256':manifest['configuration']['selection_context_sha256'],'selection_source':manifest['configuration']['selection_context']['selection_source']})
        if set(r) != allowed or type(r['schema_version']) is not int or type(r['repetition']) is not int or any(r[k] != v for k,v in expected.items()):
            raise schema2.Invalid(f'reservation contradicts frozen experiment: {path.name}')
        if runnable:
            import sys
            sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'adapters'))
            import launch
            import schema2_projection
            from harbor.models.job.config import JobConfig
            from harbor.models.task.task import Task
            if r['runnable_contract'] != manifest['runnable_contract']: raise schema2.Invalid('reservation contract mismatch')
            policy=manifest['configuration']['config']['native_config']; schema2_runnable.closed(policy,schema2_runnable.POLICY_FIELDS)
            binding=r['concrete_binding']; schema2_runnable.closed(binding,schema2_runnable.BINDING_FIELDS)
            paths=binding['path_bindings']; schema2_runnable.closed(paths,schema2_runnable.PATHS)
            for pair in paths.values(): schema2_runnable.path_pair(pair)
            for key in schema2_runnable.ROOTS:
                if not schema2_projection.exact(paths[key],policy['roots'][key]): raise schema2.Invalid('report root mismatch')
            case=root/'variants'/variant['variant_id']/'case-instance'; candidate=root/'variants'/variant['variant_id']/'candidate'
            if paths['experiment']['raw']!=str(root.absolute()) or paths['candidate']['raw']!=str(candidate) or paths['case_instance']['raw']!=str(case) or paths['evidence']['raw']!=str(Path(paths['jobs']['raw'])/('planar-t044-'+eid)):
                raise schema2.Invalid('report selected path mismatch')
            job=JobConfig.model_validate(launch.config_for('planar-t044-'+eid,Path(paths['evidence']['raw']),case,variant['effort']))
            prep=binding['verifier_preparation']
            actual=schema2_runnable.derive_binding(job,execution_id=eid,effort=variant['effort'],paths=paths,caps=manifest['configuration']['config']['caps'],original_verifier_environment=Task(case).config.verifier.environment,verifier_preparation=prep)
            _,normalized=schema2_projection.normalize(job,execution_id=eid,evidence_raw=paths['evidence']['raw'],case_raw=str(case),effort=variant['effort'])
            if not schema2_projection.exact(actual,binding) or not schema2_projection.exact(normalized,policy['normalized_job_config']): raise schema2.Invalid('report concrete binding mismatch')
            if any(prep[k]!=variant[name] for k,name in (('case_instance_sha256','case_instance_sha256'),('trusted_inputs_sha256','trusted_inputs_sha256'),('verifier_code_sha256','verifier_code_sha256'),('agent_image_id','image_id'),('verifier_image_id','verifier_image_id'))): raise schema2.Invalid('report preparation mismatch')
        predecessor, reason = r['predecessor_execution_id'], r['retry_reason_sha256']
        if predecessor is None:
            if reason is not None: raise schema2.Invalid('primary retry reason')
        elif not isinstance(predecessor,str) or len(predecessor) != 32 or any(c not in '0123456789abcdef' for c in predecessor) or not isinstance(reason,str) or not per_variant.HEX.fullmatch(reason):
            raise schema2.Invalid('invalid retry identity')
        if schema2.utc(r['expires_at']) > schema2.utc(manifest['deadline_at']): raise schema2.Invalid('reservation expiry exceeds deadline')
        folder = root/'executions'/eid
        attempt = {'reservation_id':eid, 'predecessor_execution_id':r.get('predecessor_execution_id'), 'state':'planned', 'native_start_record_present_unverified':False, 'recorded_terminal':None, **({'started':False,'outcome':'not_started','reconciliation':'not_started'} if runnable else {})}
        if folder.is_symlink(): raise schema2.Invalid('unsafe execution entry')
        if folder.exists():
            if not folder.is_dir(): raise schema2.Invalid('unsafe execution entry')
            start = schema2.read_record(folder/'start.json')
            if start.get('execution_id') != eid or start.get('logical_id') != trial['logical_id']:
                raise schema2.Invalid('execution start mismatch')
            check_optional_linkage(start, r)
            if runnable and 'concrete_job_config_sha256' in start and start['concrete_job_config_sha256'] != r['concrete_binding']['concrete_job_config_sha256']:
                raise schema2.Invalid('contradictory execution concrete identity')
            attempt['state'] = 'execution_record_present_unverified'
            if runnable: attempt.update(started=None,outcome='unknown',reconciliation='unverified')
            native = folder/'native-start.json'
            if native.exists() or native.is_symlink():
                native_record = schema2.read_record(native)
                check_optional_linkage(native_record, r)
                attempt['native_start_record_present_unverified'] = True
            if runnable:
                dispatch=folder/'dispatch.json'
                if dispatch.exists() or dispatch.is_symlink():
                    d=schema2.read_record(dispatch)
                    evidence=Path(d['evidence']); expected_evidence=Path(r['concrete_binding']['path_bindings']['evidence']['raw'])
                    if evidence!=expected_evidence or d.get('launcher_id')!='planar-t044-'+eid or evidence.is_symlink(): raise schema2.Invalid('unsafe native evidence path')
                    native=evidence/'native-trial-start.json'
                    if native.exists() or native.is_symlink():
                        if native.is_symlink() or not native.is_file(): raise schema2.Invalid('unsafe native start')
                        with native.open() as stream: n=json.load(stream,object_pairs_hook=__import__('schema2_admission').no_duplicates)
                        if not isinstance(n,dict) or not isinstance(n.get('trial_name'),str) or Path(n['trial_name']).name!=n['trial_name'] or Path(n.get('job_dir',''))!=evidence/('planar-t044-'+eid) or not all(isinstance(n.get(k),str) and n[k] for k in ('job_id','trial_id')): raise schema2.Invalid('invalid native start linkage')
                        attempt['native_start_record_present_unverified']=True
            terminal = folder/'terminal.json'
            if terminal.exists() or terminal.is_symlink():
                record = schema2.read_record(terminal)
                check_optional_linkage(record, r)
                if record.get('status') not in ('pass','behavioral_failure','incomplete','error'):
                    raise schema2.Invalid('invalid terminal status')
                attempt['recorded_terminal'] = record
            if runnable:
                receipt_path=folder/'reconciliation.json'
                if receipt_path.exists() or receipt_path.is_symlink():
                    try:
                        import schema2_execution
                        receipt=schema2.read_record(receipt_path)
                        required={'schema_version','experiment_identity','execution_id','kind','status','reason','stop_account','gate_path','sources'}
                        if set(receipt)!=required or type(receipt['schema_version']) is not int or receipt['schema_version']!=1 or receipt['experiment_identity']!=identity or receipt['execution_id']!=eid or receipt['kind'] not in ('native','pre-dispatch') or receipt['status'] not in ('pass','behavioral_failure','incomplete','error') or type(receipt['stop_account']) is not bool: raise ValueError('invalid reconciliation receipt')
                        gate=Path(receipt['gate_path']); review_root=Path(policy['roots']['reviews']['resolved'])
                        if gate.is_symlink() or not gate.resolve().is_relative_to(review_root): raise ValueError('unsafe receipt gate')
                        gate_object=per_variant.parse(gate.read_bytes(),1024*1024)
                        import hashlib
                        marker_hash=hashlib.sha256(json.dumps(gate_object,sort_keys=True,separators=(',',':')).encode()).hexdigest()
                        consumed=Path(policy['roots']['consumed']['resolved']); marker=consumed/marker_hash
                        if start.get('gate_marker_sha256')!=marker_hash or consumed.is_symlink() or marker.is_symlink() or not marker.is_file() or marker.read_bytes()!=b'reserved\n': raise ValueError('receipt gate is not the consumed gate')
                        if receipt['sources']!=schema2_execution.receipt_sources(root,r,gate,receipt['kind']): raise ValueError('reconciliation sources changed')
                        if attempt['recorded_terminal'] is None or any(attempt['recorded_terminal'].get(k)!=receipt[k] for k in ('status','reason')): raise ValueError('terminal contradicts receipt')
                        if receipt['kind']=='native':
                            result=schema2.read_record(folder/'native-result.json'); classification=result['classification']
                            if any(classification.get(k)!=receipt[k] for k in ('status','reason','stop_account')) or not attempt['native_start_record_present_unverified']: raise ValueError('native result contradicts receipt')
                            attempt['started']=True
                        else:
                            if receipt['status']!='incomplete' or attempt['native_start_record_present_unverified']: raise ValueError('invalid pre-dispatch receipt')
                            attempt['started']=False
                        records=Path(manifest['configuration']['account_scope_id'])/'records'
                        shared={'experiment':str(root.resolve()),'execution_id':eid,'runnable_contract':r['runnable_contract'],'reservation_sha256':schema2.h(path.read_bytes()),'gate_marker_sha256':marker_hash,'gate_raw_sha256':schema2.h(gate.read_bytes())}
                        if r['runnable_contract']==schema2_runnable.CONTRACT: shared.update({k:r[k] for k in schema2_runnable.CONTEXT_FIELDS})
                        if records.is_symlink() or schema2.read_record(records/f'{eid}.start.json')!=shared: raise ValueError('shared admission mismatch')
                        if not receipt['stop_account']:
                            closed=records/f'{eid}.closed.json'
                            if schema2.read_record(closed)!={'status':receipt['status']}: raise ValueError('shared closure missing')
                        attempt.update(outcome=receipt['status'],reconciliation='validated')
                    except (OSError,ValueError,KeyError,TypeError):
                        attempt.update(started=None,outcome='unknown',reconciliation='invalid_or_incomplete')
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
        counts={'variant_id':variant['variant_id'], 'planned_logical_trials':len(rows),
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
            'retry_attempts':sum(max(0,len(row['attempts'])-1) for row in rows)}
        if runnable:
            counts['primary_started']=sum(bool(x and x['started'] is True) for x in primary)
            counts['primary_not_started']=sum(x is None or x['started'] is False for x in primary)
            counts['all_attempts_started']=sum(x['started'] is True for x in all_attempts)
            counts['all_attempts_not_started']=sum(x['started'] is False for x in all_attempts)
            for state in ('pass','behavioral_failure','incomplete','error','unknown'):
                counts['primary_'+state]=sum(bool(x and x['outcome']==state) for x in primary)
                counts['all_attempts_'+state]=sum(x['outcome']==state for x in all_attempts)
        groups.append(counts)
    return {'schema_version':2, 'experiment_identity':identity,
        'runnable_contract':manifest.get('runnable_contract'),
        'assessment':('only source-bound reconciliation receipts count as outcomes; unverified records remain unknown' if runnable else 'unreconciled: reservation and recorded execution inventory only; gate, preparation evidence and behavioral verdict not inferred'),
        'groups':groups, 'trials':list(trials.values())}
