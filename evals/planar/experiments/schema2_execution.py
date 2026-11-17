"""Schema-2 admission primitives. No authentication or dispatch occurs here."""
import os
import json
from pathlib import Path
import schema2


def _sync_directory(path):
    fd=os.open(path,os.O_RDONLY)
    try: os.fsync(fd)
    finally: os.close(fd)


def implementation_identity():
    """Identity the existing guarded launcher and experiment source separately."""
    import sys
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'adapters'))
    import launch, controls
    return schema2.object_hash({'launcher':launch.source_digest(), 'experiments':controls.tree_digest(Path(__file__).resolve().parent)})


def activate_runnable(root, identity, reservation_id, gate_path, *, preparation_evidence, _dispatch):
    """Consume a gate and run the guarded dispatcher while holding both locks."""
    import fcntl, stat, subprocess, shutil, sys
    import per_variant as pv
    import schema2_runnable
    import codex_run
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'adapters'))
    import launch
    root=Path(root); gate_path=Path(gate_path); pv.safe_id(reservation_id)
    if root.is_symlink() or not root.is_dir(): raise schema2.Invalid('unsafe experiment root')
    reviews=launch.reviews_root()
    if reviews.is_symlink() or not reviews.is_dir() or gate_path.is_symlink() or not gate_path.is_file() or not gate_path.resolve().is_relative_to(reviews.resolve()): raise schema2.Invalid('unsafe gate or review root')
    scope=reviews/'planar-account-admission-v1'
    if scope.is_symlink() or (scope/'lock').is_symlink() or (scope/'records').is_symlink(): raise schema2.Invalid('unsafe shared admission scope')
    # Use the same account lock and account record namespace as schema 1.
    with codex_run.admission_scope() as records:
        try: fd=os.open(root/'controller.lock',os.O_RDWR|os.O_CREAT|os.O_NOFOLLOW,0o600)
        except OSError as exc: raise schema2.Invalid('unsafe controller lock') from exc
        with os.fdopen(fd,'r+') as lock:
            if not stat.S_ISREG(os.fstat(lock.fileno()).st_mode): raise schema2.Invalid('unsafe controller lock')
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            if codex_run.shared_pending(records) or codex_run.pending_account_stops(root): raise schema2.Invalid('shared account admission blocked')
            gate_bytes=gate_path.read_bytes()
            gate=pv.parse(gate_bytes,1024*1024)
            checked=schema2_runnable.check_gate(root,identity,reservation_id,gate,preparation_evidence=preparation_evidence)
            if checked['status'] != 'review-only': raise schema2.Invalid('gate comparison incomplete')
            reservation_path=root/'reservations'/f'{reservation_id}.json'
            reservation_bytes=reservation_path.read_bytes(); r=schema2.read_record(reservation_path)
            if r['account_scope_id'] != str(records.parent.resolve()) or r['implementation_sha256'] != implementation_identity(): raise schema2.Invalid('account scope or implementation drift')
            if (root/'reservations'/f'{reservation_id}.cancelled.json').exists(): raise schema2.Invalid('reservation cancelled')
            docker=shutil.which('docker'); codex=shutil.which('codex')
            if not docker or not codex: raise schema2.Invalid('required no-auth inspection unavailable')
            for image_id in (r['image_id'],r['verifier_image_id']):
                result=subprocess.run([docker,'image','inspect','--format','{{.Id}}',image_id],capture_output=True,text=True,timeout=15)
                if result.returncode or result.stdout.strip()!=image_id: raise schema2.Invalid('reviewed image unavailable')
            version=subprocess.run([codex,'--version'],capture_output=True,text=True,timeout=15)
            if version.returncode or version.stdout.strip()!=r['host_version']: raise schema2.Invalid('host version mismatch')
            # Inspection may take time: repeat full review, deadline and scope
            # checks before acquiring any shared account record.
            if gate_path.is_symlink() or gate_path.read_bytes()!=gate_bytes or reservation_path.read_bytes()!=reservation_bytes: raise schema2.Invalid('review bytes drift')
            schema2_runnable.check_gate(root,identity,reservation_id,gate,preparation_evidence=preparation_evidence)
            marker_hash=__import__('hashlib').sha256(json.dumps(gate,sort_keys=True,separators=(',',':')).encode()).hexdigest()
            consumed=reviews/'harbor-t044-consumed'
            if consumed.is_symlink() or (consumed/marker_hash).exists(): raise schema2.Invalid('gate already consumed or unsafe')
            folder=root/'executions'/reservation_id
            if folder.exists() or folder.is_symlink(): raise schema2.Invalid('reservation already activated')
            schema2.write_new(records/f'{reservation_id}.start.json',{'experiment':str(root.resolve()),'execution_id':reservation_id,'runnable_contract':'planar-schema2-runnable-v1','reservation_sha256':schema2.h(reservation_bytes),'gate_marker_sha256':marker_hash,'gate_raw_sha256':schema2.h(gate_bytes)})
            folder.mkdir(mode=0o700)
            _sync_directory(folder.parent)
            schema2.write_new(folder/'start.json',{'execution_id':reservation_id,'reservation_id':reservation_id,'logical_id':r['logical_id'],'variant_id':r['variant_id'],'repetition':r['repetition'],'predecessor_execution_id':r['predecessor_execution_id'],'reservation_sha256':schema2.h(reservation_bytes),'gate_marker_sha256':marker_hash,'concrete_job_config_sha256':r['concrete_binding']['concrete_job_config_sha256']})
            # Any post-start failure leaves the shared record pending.
            if gate_path.is_symlink() or gate_path.read_bytes()!=gate_bytes or reservation_path.read_bytes()!=reservation_bytes: raise schema2.Invalid('review bytes drift after start')
            schema2.validate(root,identity)
            now=schema2.datetime.now(schema2.timezone.utc)
            required=min(r['concrete_binding']['caps']['trial_seconds'],1500)
            if min(schema2.utc(gate['expires_at']),schema2.utc(r['expires_at']),schema2.utc(schema2.read_record(root/'manifest.json')['deadline_at']))-now < schema2.timedelta(seconds=required): raise schema2.Invalid('insufficient remaining reviewed wall time')
            if consumed.is_symlink(): raise schema2.Invalid('unsafe consumed root')
            consumed.mkdir(mode=0o700,exist_ok=True)
            _sync_directory(consumed.parent)
            try: marker_fd=os.open(consumed/marker_hash,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
            except FileExistsError: raise schema2.Invalid('gate already consumed') from None
            with os.fdopen(marker_fd,'w') as output:
                output.write('reserved\n'); output.flush(); os.fsync(output.fileno())
            dirfd=os.open(consumed,os.O_RDONLY)
            try: os.fsync(dirfd)
            finally: os.close(dirfd)
            launcher_id='planar-t044-'+reservation_id
            evidence=launch.jobs_root()/launcher_id
            trial_deadline=min(r['concrete_binding']['caps']['trial_seconds'],1500)
            schema2.write_new(folder/'dispatch.json',{'launcher_id':launcher_id,'evidence':str(evidence),'case':'native-lineage-portable','effort':r['concrete_binding']['job_config']['agents'][0]['kwargs']['reasoning_effort'],'trial_deadline':trial_deadline})
            # Keep both locks through guarded launch, so recovery cannot mistake
            # a live dispatcher for a crash before the durable dispatch boundary.
            return _dispatch()


def verify_activated(root, identity, reservation_id, gate_path, *, preparation_evidence):
    """Recheck consumed runnable admission and frozen inputs; never read auth."""
    import hashlib, shutil, subprocess, sys
    import per_variant as pv
    import schema2_runnable
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'adapters'))
    import launch
    root=Path(root); gate_path=Path(gate_path); pv.safe_id(reservation_id)
    reviews=launch.reviews_root()
    if reviews.is_symlink() or gate_path.is_symlink() or not gate_path.is_file() or not gate_path.resolve().is_relative_to(reviews.resolve()): raise schema2.Invalid('unsafe gate')
    gate_bytes=gate_path.read_bytes(); gate=pv.parse(gate_bytes,1024*1024)
    result=schema2_runnable.check_gate(root,identity,reservation_id,gate,preparation_evidence=preparation_evidence,allow_activated=True)
    reservation_path=root/'reservations'/f'{reservation_id}.json'; r=schema2.read_record(reservation_path)
    folder=root/'executions'/reservation_id
    if folder.is_symlink() or not folder.is_dir(): raise schema2.Invalid('missing activated execution')
    start=schema2.read_record(folder/'start.json')
    marker_hash=hashlib.sha256(json.dumps(gate,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    if start.get('execution_id')!=reservation_id or start.get('reservation_id')!=reservation_id or start.get('logical_id')!=r['logical_id'] or start.get('reservation_sha256')!=schema2.h(reservation_path.read_bytes()) or start.get('gate_marker_sha256')!=marker_hash or start.get('concrete_job_config_sha256')!=r['concrete_binding']['concrete_job_config_sha256']:
        raise schema2.Invalid('activated execution mismatch')
    scope=reviews/'planar-account-admission-v1'; records=scope/'records'; shared=records/f'{reservation_id}.start.json'
    if scope.is_symlink() or records.is_symlink() or shared.is_symlink() or r['account_scope_id']!=str(scope.resolve()): raise schema2.Invalid('unsafe shared admission')
    if schema2.read_record(shared)!={'experiment':str(root.resolve()),'execution_id':reservation_id,'runnable_contract':'planar-schema2-runnable-v1','reservation_sha256':schema2.h(reservation_path.read_bytes()),'gate_marker_sha256':marker_hash,'gate_raw_sha256':schema2.h(gate_bytes)} or (records/f'{reservation_id}.closed.json').exists() or (records/f'{reservation_id}.resolved.json').exists(): raise schema2.Invalid('shared admission not active')
    consumed=reviews/'harbor-t044-consumed'; marker=consumed/marker_hash
    if consumed.is_symlink() or marker.is_symlink() or not marker.is_file() or marker.read_bytes()!=b'reserved\n': raise schema2.Invalid('gate consumption missing')
    if r['implementation_sha256']!=implementation_identity(): raise schema2.Invalid('implementation changed')
    docker=shutil.which('docker'); codex=shutil.which('codex')
    if not docker or not codex: raise schema2.Invalid('required inspection unavailable')
    for image_id in (r['image_id'],r['verifier_image_id']):
        inspection=subprocess.run([docker,'image','inspect','--format','{{.Id}}',image_id],capture_output=True,text=True,timeout=15)
        if inspection.returncode or inspection.stdout.strip()!=image_id: raise schema2.Invalid('reviewed image unavailable')
    version=subprocess.run([codex,'--version'],capture_output=True,text=True,timeout=15)
    if version.returncode or version.stdout.strip()!=r['host_version']: raise schema2.Invalid('host version mismatch')
    if gate_path.read_bytes()!=gate_bytes or reservation_path.read_bytes()!=pv.canonical(r): raise schema2.Invalid('review drift')
    return {'reservation':r,'gate':gate,'concrete_job_config_sha256':result['concrete_job_config_sha256']}


def execute_runnable(root, identity, reservation_id, auth_path, gate_path, *, preparation_evidence):
    """Activate once and use the existing guarded launcher; reconciliation pending."""
    import sys
    import per_variant as pv
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'adapters'))
    import launch
    root=Path(root); pv.safe_id(reservation_id)
    r=schema2.read_record(root/'reservations'/f'{reservation_id}.json')
    if r.get('runnable_contract') != 'planar-schema2-runnable-v1': raise schema2.Invalid('offline-only reservation')
    selected=schema2.preflight(root,identity,r['logical_id'])
    caps=r['concrete_binding']['caps']
    trial_deadline=min(caps['trial_seconds'],1500)
    remaining=(schema2.utc(r['expires_at'])-schema2.datetime.now(schema2.timezone.utc)).total_seconds()
    if remaining < trial_deadline: raise schema2.Invalid('insufficient reviewed deadline')
    folder=root/'executions'/reservation_id
    launcher_id='planar-t044-'+reservation_id
    def dispatch():
        try:
            launched=launch.execute(Path(auth_path),Path(gate_path),'native-lineage-portable',candidate_root=Path(selected['candidate']),case_root=Path(selected['instance']),trial_deadline=trial_deadline,execution_id=launcher_id,effort=selected['variant']['effort'],absolute_deadline=r['expires_at'],emit=False,schema2_context={'root':str(root),'identity':identity,'reservation_id':reservation_id,'preparation_evidence':str(preparation_evidence)})
        except Exception as exc:
            schema2.write_new(folder/'account-stop.json',{'reason':'guarded portable launcher failure'})
            schema2.write_new(folder/'terminal.json',{'status':'error','reason':'guarded portable launcher failed: '+type(exc).__name__})
            return {'execution_id':reservation_id,'status':'error','stop_account':True}
        schema2.write_new(folder/'launcher-result.json',launched)
        return None
    early=activate_runnable(root,identity,reservation_id,gate_path,preparation_evidence=preparation_evidence,_dispatch=dispatch)
    if early is not None: return early
    outcome=reconcile_runnable(root,identity,reservation_id,gate_path)
    if outcome is None:
        return _record_unreconciled(root,reservation_id)
    return {'execution_id':reservation_id,**outcome}


def _record_unreconciled(root, reservation_id):
    """Record a conservative stop without racing a concurrent reconciler."""
    import codex_run
    root=Path(root); folder=root/'executions'/reservation_id
    with codex_run.admission_scope() as records, _controller_lock(root):
        terminal=folder/'terminal.json'; closed=records/f'{reservation_id}.closed.json'
        if terminal.exists() or terminal.is_symlink() or closed.exists() or closed.is_symlink():
            # Another reconciler owns that immutable state; do not manufacture
            # a conflicting stop/terminal after its lock-protected decision.
            return {'execution_id':reservation_id,'status':'incomplete','stop_account':not closed.is_file()}
        stop=folder/'account-stop.json'
        if stop.exists() or stop.is_symlink():
            if schema2.read_record(stop)!={'reason':'portable native provenance or cleanup unconfirmed'}: raise schema2.Invalid('conflicting account stop')
        else: schema2.write_new(stop,{'reason':'portable native provenance or cleanup unconfirmed'})
        schema2.write_new(terminal,{'status':'incomplete','reason':'portable native provenance or cleanup unconfirmed'})
        return {'execution_id':reservation_id,'status':'incomplete','stop_account':True}


def reconcile_runnable(root, identity, reservation_id, gate_path):
    """Reconcile portable provenance before invoking existing native reconciler."""
    import fcntl, sys
    import codex_run, controls
    import schema2_projection, schema2_runnable, per_variant as pv, hashlib
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'adapters'))
    import launch
    from smoke_codex import CACHE, CONFIG
    from harbor.models.job.config import JobConfig
    root=Path(root)
    if not isinstance(reservation_id,str) or len(reservation_id)!=32 or any(c not in '0123456789abcdef' for c in reservation_id) or root.is_symlink() or not root.is_dir(): return None
    folder=root/'executions'/reservation_id
    try:
        with codex_run.admission_scope() as records, _controller_lock(root) as lock:
            manifest=schema2.validate(root,identity)
            reservation_path=root/'reservations'/f'{reservation_id}.json'
            r=schema2.read_record(reservation_path); schema2_runnable.closed(r,schema2_runnable.RESERVATION_FIELDS)
            if r.get('runnable_contract')!='planar-schema2-runnable-v1' or r['experiment_identity']!=identity or r['reservation_id']!=reservation_id or r['execution_id']!=reservation_id: raise ValueError('reservation mismatch')
            v=next(x for x in manifest['variants'] if x['variant_id']==r['variant_id'])
            if any(r[key]!=v[key] for key in (*schema2.IDENTITIES,'configuration_sha256','image_id','verifier_image_id','host_version')): raise ValueError('frozen reservation mismatch')
            binding=r['concrete_binding']; schema2_runnable.closed(binding,schema2_runnable.BINDING_FIELDS)
            concrete=schema2_runnable.digest({k:value for k,value in binding.items() if k!='concrete_job_config_sha256'})
            if binding['concrete_job_config_sha256']!=concrete: raise ValueError('concrete binding mismatch')
            gate_file=Path(gate_path)
            if gate_file.is_symlink() or not gate_file.is_file() or not gate_file.resolve().is_relative_to(launch.reviews_root().resolve()): raise ValueError('gate missing or outside review scope')
            gate=pv.parse(gate_file.read_bytes(),1024*1024); schema2_runnable.closed(gate,schema2_runnable.GATE_FIELDS)
            expected={k:r[k] for k in schema2.GATE_FIELDS if k in r and k not in ('schema_version','expires_at')}
            expected.update({'schema_version':2,'purpose':'one-experiment-trial','verdict':'approve','max_attempts':1,'reservation_sha256':schema2.h(reservation_path.read_bytes()),'runnable_contract':'planar-schema2-runnable-v1','concrete_job_config_sha256':concrete})
            if any(gate.get(k)!=value for k,value in expected.items()) or schema2.utc(gate['expires_at'])>schema2.utc(r['expires_at']): raise ValueError('gate mismatch')
            if (type(gate.get('schema_version')) is not int or type(gate.get('max_attempts')) is not int
                or not all(isinstance(gate.get(key),str) and gate[key].strip() for key in ('reviewer','report','reviewed_at'))
                or schema2.utc(gate['reviewed_at']) > schema2.utc(gate['expires_at'])
                or schema2.utc(gate['expires_at']) > schema2.utc(manifest['deadline_at'])): raise ValueError('gate review metadata mismatch')
            if r['account_scope_id'] != str(records.parent.resolve()) or r['account_scope_id'] != manifest['configuration']['account_scope_id'] or r['canonical_output_root'] != manifest['configuration']['canonical_output_root'] or r['implementation_sha256'] != manifest['implementation_sha256'] or r['state'] != 'planned': raise ValueError('reservation scope mismatch')
            marker_hash=hashlib.sha256(json.dumps(gate,sort_keys=True,separators=(',',':')).encode()).hexdigest()
            expected_shared={'experiment':str(root.resolve()),'execution_id':reservation_id,'runnable_contract':'planar-schema2-runnable-v1','reservation_sha256':schema2.h(reservation_path.read_bytes()),'gate_marker_sha256':marker_hash,'gate_raw_sha256':schema2.h(gate_file.read_bytes())}
            if schema2.read_record(records/f'{reservation_id}.start.json')!=expected_shared: raise ValueError('account record mismatch or unbound legacy start')
            consumed=launch.reviews_root()/'harbor-t044-consumed'; marker=consumed/marker_hash
            if consumed.is_symlink() or marker.is_symlink(): raise ValueError('unsafe gate marker')
            launcher_id='planar-t044-'+reservation_id; expected_evidence=launch.jobs_root()/launcher_id
            if folder.is_symlink(): raise ValueError('unsafe local execution')
            if folder.exists() and not folder.is_dir(): raise ValueError('unsafe local execution')
            dispatch_path=folder/'dispatch.json'
            has_dispatch=dispatch_path.exists() or dispatch_path.is_symlink()
            start_path=folder/'start.json'; has_start=start_path.exists() or start_path.is_symlink()
            marker_exists=marker.exists()
            if not has_start or not marker_exists:
                # The account lock proves activation is no longer in its locked
                # write sequence. Only its known pre-dispatch prefixes can be
                # completed; any later evidence leaves this account blocked.
                if has_dispatch or expected_evidence.exists() or expected_evidence.is_symlink() or (not has_start and marker_exists): raise ValueError('ambiguous early admission state')
                if folder.exists() and any(p.name != 'start.json' for p in folder.iterdir()): raise ValueError('unexpected early execution evidence')
                closed=records/f'{reservation_id}.closed.json'; resolved=records/f'{reservation_id}.resolved.json'
                if closed.exists() or closed.is_symlink() or resolved.exists() or resolved.is_symlink(): raise ValueError('early admission already resolved')
                if not folder.exists():
                    folder.mkdir(mode=0o700)
                    _sync_directory(folder.parent)
            expected_start={'execution_id':reservation_id,'reservation_id':reservation_id,'logical_id':r['logical_id'],
                'variant_id':r['variant_id'],'repetition':r['repetition'],'predecessor_execution_id':r['predecessor_execution_id'],
                'reservation_sha256':schema2.h(reservation_path.read_bytes()),'gate_marker_sha256':marker_hash,'concrete_job_config_sha256':concrete}
            if has_start:
                if schema2.read_record(start_path)!=expected_start: raise ValueError('local start mismatch')
            else: schema2.write_new(start_path,expected_start)
            if not marker_exists:
                # Consume the independently reviewed gate once even when the
                # original process crashed before its marker write. This is a
                # recovery action, not authorization to dispatch or read auth.
                consumed.mkdir(mode=0o700,exist_ok=True)
                _sync_directory(consumed.parent)
                try: marker_fd=os.open(marker,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
                except FileExistsError: raise ValueError('gate marker race') from None
                with os.fdopen(marker_fd,'w') as out:
                    out.write('reserved\n'); out.flush(); os.fsync(out.fileno())
                dirfd=os.open(consumed,os.O_RDONLY)
                try: os.fsync(dirfd)
                finally: os.close(dirfd)
            if not marker.is_file() or marker.read_bytes()!=b'reserved\n': raise ValueError('gate consumption mismatch')
            if not dispatch_path.exists() and not dispatch_path.is_symlink():
                if (folder/'account-stop.json').exists() or (folder/'account-stop.json').is_symlink(): raise ValueError('unresolved account stop')
                if expected_evidence.exists() or expected_evidence.is_symlink(): raise ValueError('ambiguous pre-dispatch evidence')
                reason='activation ended before dispatch; no launcher evidence exists'
                recovery=folder/'pre-dispatch-recovery.json'
                expected_recovery={'execution_id':reservation_id,'reservation_sha256':schema2.h(reservation_path.read_bytes()),'gate_marker_sha256':marker_hash,'status':'incomplete','reason':reason}
                if recovery.exists() or recovery.is_symlink():
                    if schema2.read_record(recovery)!=expected_recovery: raise ValueError('recovery contradiction')
                else: schema2.write_new(recovery,expected_recovery)
                terminal=folder/'terminal.json'
                if terminal.exists() or terminal.is_symlink():
                    existing=schema2.read_record(terminal)
                    if existing.get('status')!='incomplete' or existing.get('reason')!=reason: raise ValueError('terminal contradiction')
                else: controls.finish(root,reservation_id,'incomplete',reason)
                _write_receipt(root,identity,r,gate_file,'pre-dispatch',{'status':'incomplete','reason':reason,'stop_account':False})
                closed=records/f'{reservation_id}.closed.json'
                if closed.exists() or closed.is_symlink():
                    if schema2.read_record(closed)!={'status':'incomplete'}: raise ValueError('closure contradiction')
                else: schema2.write_new(closed,{'status':'incomplete'})
                return {'status':'incomplete','reason':reason,'stop_account':False}
            dispatch=schema2.read_record(dispatch_path); evidence=Path(dispatch['evidence'])
            if evidence != launch.jobs_root()/launcher_id or evidence.is_symlink() or not evidence.is_dir() or dispatch['launcher_id']!=launcher_id: raise ValueError('evidence mismatch')
            def read(name):
                path=evidence/name
                if path.is_symlink() or not path.is_file() or path.stat().st_size>1024*1024: raise ValueError('missing provenance')
                with path.open() as f: value=json.load(f,object_pairs_hook=__import__('schema2_admission').no_duplicates)
                if not isinstance(value,dict): raise ValueError('invalid provenance')
                return value
            launched=read('launcher.json'); native=read('native-trial-start.json')
            instance=Path(r['concrete_binding']['path_bindings']['case_instance']['raw'])
            if (launched.get('execution_id')!=launcher_id or launched.get('case')!='native-lineage-portable' or launched.get('runnable_contract')!='planar-schema2-runnable-v1' or launched.get('case_sha256')!=launch.case_inventory(instance) or launched.get('candidate_sha256')!=r['candidate_inventory_sha256'] or launched.get('image_id')!=r['image_id'] or launched.get('verifier_image_id')!=r['verifier_image_id'] or launched.get('review_sha256')!=schema2.h(Path(gate_path).read_bytes())): raise ValueError('launcher provenance mismatch')
            job_dir=evidence/launcher_id; trial_name=native['trial_name']
            if not isinstance(trial_name,str) or Path(trial_name).name!=trial_name: raise ValueError('unsafe trial name')
            submitted=JobConfig.model_validate_json((evidence/'job-config.json').read_text())
            actual=JobConfig.model_validate_json((job_dir/'config.json').read_text())
            expected=r['concrete_binding']['job_config']
            if not schema2_projection.exact(schema2_projection.project(submitted),expected) or not schema2_projection.exact(schema2_projection.project(actual),expected): raise ValueError('native config mismatch')
            from harbor.models.trial.result import TrialResult
            trial=TrialResult.model_validate_json((job_dir/trial_name/'result.json').read_text())
            has_errors=bool(trial.recorded_exceptions())
            # Errors can precede verifier or agent setup. Missing phase evidence
            # is allowed only for typed native errors; any supplied record must
            # still agree with the reviewed execution and input identities.
            names=('planar-container.json','planar-image.json','planar-install-provenance.json','planar-workspace-preflight.json','planar-cleanup.json','planar-cli.json','verifier-network.json','verifier-compose-up.json')
            observations={}
            for name in names:
                path=evidence/name
                if path.exists() or path.is_symlink():
                    observations[name]=read(name)
                    if observations[name].get('execution_id')!=launcher_id: raise ValueError('provenance execution mismatch')
                elif not has_errors: raise ValueError('missing successful execution provenance')
            container=observations.get('planar-container.json'); image=observations.get('planar-image.json'); installed=observations.get('planar-install-provenance.json'); workspace=observations.get('planar-workspace-preflight.json'); cleanup=observations.get('planar-cleanup.json'); cli=observations.get('planar-cli.json'); network=observations.get('verifier-network.json'); compose=observations.get('verifier-compose-up.json')
            if container is not None and container.get('image_id')!=r['image_id']: raise ValueError('agent image mismatch')
            if image is not None and image.get('id')!=r['image_id']: raise ValueError('image provenance mismatch')
            if installed is not None and (installed.get('candidate_sha256')!=r['candidate_inventory_sha256'] or installed.get('installed_path')!=CACHE or installed.get('plugin_id')!='planar@planar-smoke-market' or installed.get('list_contains_plugin') is not True or installed.get('effective_config',{}).get('plugins')!={'_default':{'enabled':False},'planar@planar-smoke-market':{'enabled':True}} or installed.get('effective_config',{}).get('features')!=CONFIG['features'] or installed.get('effective_config',{}).get('skills')!=CONFIG['skills']): raise ValueError('installation provenance mismatch')
            if workspace is not None and (workspace.get('status')!='passed' or workspace.get('check')!='empty-directory-before-model' or workspace.get('fixture')!='/workspace'): raise ValueError('workspace provenance mismatch')
            if cleanup is not None and cleanup.get('status')!='passed': raise ValueError('agent cleanup contradiction')
            if cli is not None and cli.get('version')!=r['host_version']: raise ValueError('CLI version mismatch')
            if network is not None and (network.get('network_mode')!='none' or network.get('image_id')!=r['verifier_image_id']): raise ValueError('verifier network or image mismatch')
            if compose is not None and compose.get('command',[])[-3:]!=['--no-build','--pull','never']: raise ValueError('verifier compose mismatch')
            status_path=job_dir/trial_name/'verifier/status.json'
            if status_path.exists() or status_path.is_symlink():
                if status_path.is_symlink() or not status_path.is_file(): raise ValueError('unsafe verifier status')
                status=json.loads(status_path.read_text(),object_pairs_hook=__import__('schema2_admission').no_duplicates)
                if status.get('case')!='planar/native-lineage-portable' or status.get('contract_version')!=2: raise ValueError('verifier identity mismatch')
                if status.get('status') not in ('pass','behavioral_failure','incomplete','infrastructure_error'): raise ValueError('invalid verifier status')
                if has_errors and status.get('status') in ('pass','behavioral_failure'): raise ValueError('native error contradicts behavioral verifier status')
                reward_path=job_dir/trial_name/'verifier/reward.txt'
                if reward_path.exists() or reward_path.is_symlink():
                    if reward_path.is_symlink() or not reward_path.is_file() or reward_path.stat().st_size>128: raise ValueError('unsafe verifier reward')
                    reward=reward_path.read_text().strip()
                    if reward not in ('0','1') or (reward=='1') != (status.get('status')=='pass'): raise ValueError('contradictory verifier reward')
            elif not has_errors: raise ValueError('verifier status missing')
            elif (job_dir/trial_name/'verifier/reward.txt').exists() or (job_dir/trial_name/'verifier/reward.txt').is_symlink(): raise ValueError('reward without verifier status')
            terminal=folder/'terminal.json'
            if terminal.exists() or terminal.is_symlink():
                # Re-run the existing native reconciler against scratch copies
                # of immutable start/dispatch; never rewrite retained evidence.
                import tempfile
                with tempfile.TemporaryDirectory(prefix='.schema2-reconcile-',dir=root) as temp:
                    staged=Path(temp)/'executions'/reservation_id; staged.mkdir(parents=True)
                    for name in ('start.json','dispatch.json'):
                        (staged/name).write_bytes((folder/name).read_bytes())
                    outcome=codex_run.reconcile(staged)
                    if outcome is None: return None
                    retained=schema2.read_record(folder/'native-result.json')
                    rebuilt=schema2.read_record(staged/'native-result.json')
                    old_terminal=schema2.read_record(terminal)
                    if retained!=rebuilt or old_terminal.get('status')!=outcome['status'] or old_terminal.get('reason')!=outcome['reason']:
                        raise ValueError('retained terminal evidence contradicts recomputation')
            else:
                outcome=codex_run.reconcile(folder)
            if outcome is None: return None
            _write_receipt(root,identity,r,gate_file,'native',outcome)
            if not outcome['stop_account']:
                closed=records/f'{reservation_id}.closed.json'
                if closed.exists() or closed.is_symlink():
                    if schema2.read_record(closed)!={'status':outcome['status']}: raise ValueError('closure contradiction')
                else: schema2.write_new(closed,{'status':outcome['status']})
            return outcome
    except (OSError,ValueError,KeyError,TypeError,StopIteration):
        return None


def receipt_sources(root, reservation, gate_path, kind):
    """Recompute bounded evidence hashes without changing records or running jobs."""
    import hashlib
    root=Path(root); eid=reservation['execution_id']; folder=root/'executions'/eid
    files={'reservation':root/'reservations'/f'{eid}.json','start':folder/'start.json','terminal':folder/'terminal.json','gate':Path(gate_path)}
    optional=set()
    if kind == 'pre-dispatch': files['recovery']=folder/'pre-dispatch-recovery.json'
    elif kind == 'native':
        dispatch=schema2.read_record(folder/'dispatch.json'); evidence=Path(dispatch['evidence'])
        expected=Path(reservation['concrete_binding']['path_bindings']['evidence']['raw'])
        if evidence!=expected or evidence.is_symlink() or not evidence.is_dir(): raise ValueError('unsafe receipt evidence')
        native_path=evidence/'native-trial-start.json'
        if native_path.is_symlink() or not native_path.is_file(): raise ValueError('native start missing')
        with native_path.open() as stream: native=json.load(stream,object_pairs_hook=__import__('schema2_admission').no_duplicates)
        name=native['trial_name']; job=evidence/('planar-t044-'+eid)
        if not isinstance(name,str) or Path(name).name!=name or Path(native['job_dir'])!=job: raise ValueError('native start mismatch')
        files.update({'dispatch':folder/'dispatch.json','native_result':folder/'native-result.json'})
        for name2 in ('launcher.json','planar-container.json','planar-image.json','planar-install-provenance.json','planar-workspace-preflight.json','planar-cleanup.json','planar-cli.json','verifier-network.json','verifier-compose-up.json','native-trial-start.json','launcher-terminal.json','watchdog-terminal.json','job-config.json'):
            files['evidence/'+name2]=evidence/name2
        for name2 in ('config.json','result.json'):
            files['native-job/'+name2]=job/name2
        for name2 in ('result.json','verifier/status.json','verifier/reward.txt'):
            files['native-trial/'+name2]=job/name/name2
        from harbor.models.trial.result import TrialResult
        trial=TrialResult.model_validate_json((job/name/'result.json').read_text())
        if trial.recorded_exceptions():
            optional={'evidence/'+name2 for name2 in ('planar-container.json','planar-image.json','planar-install-provenance.json','planar-workspace-preflight.json','planar-cleanup.json','planar-cli.json','verifier-network.json','verifier-compose-up.json')}
            optional |= {'native-trial/verifier/status.json','native-trial/verifier/reward.txt'}
    else: raise ValueError('unknown receipt kind')
    result={}
    for key,path in files.items():
        if key in optional and not path.exists() and not path.is_symlink():
            result[key]=None
            continue
        if kind == 'native' and path.is_relative_to(evidence): anchor=evidence
        elif path.is_relative_to(folder): anchor=folder
        elif key == 'reservation': anchor=root/'reservations'
        else: anchor=path.parent
        node=path
        while node != anchor:
            if node.is_symlink(): raise ValueError('symlink in receipt source')
            node=node.parent
        if anchor.is_symlink() or path.is_symlink() or not path.is_file() or path.stat().st_size>16*1024*1024: raise ValueError('unsafe or missing receipt source')
        result[key]=hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def _write_receipt(root, identity, reservation, gate_path, kind, outcome):
    folder=Path(root)/'executions'/reservation['execution_id']; path=folder/'reconciliation.json'
    receipt={'schema_version':1,'experiment_identity':identity,'execution_id':reservation['execution_id'],
        'kind':kind,'status':outcome['status'],'reason':outcome['reason'],'stop_account':outcome['stop_account'],
        'gate_path':str(Path(gate_path).absolute()),'sources':receipt_sources(root,reservation,gate_path,kind)}
    if path.exists() or path.is_symlink():
        if schema2.read_record(path)!=receipt: raise ValueError('reconciliation receipt contradiction')
    else: schema2.write_new(path,receipt)
    return receipt


@__import__('contextlib').contextmanager
def _controller_lock(root):
    import fcntl, stat
    fd=os.open(Path(root)/'controller.lock',os.O_RDWR|os.O_CREAT|os.O_NOFOLLOW,0o600)
    with os.fdopen(fd,'r+') as lock:
        if not stat.S_ISREG(os.fstat(lock.fileno()).st_mode): raise ValueError('unsafe controller lock')
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        yield lock


def resolve_account_stop(root, identity, reservation_id, reason, *, gate_path):
    """Record an explicit schema-2 account resolution after confirmed cleanup."""
    import hashlib, sys
    import per_variant as pv
    import schema2_runnable, codex_run
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'adapters'))
    import launch
    root=Path(root); pv.safe_id(reservation_id)
    if root.is_symlink() or not root.is_dir(): raise schema2.Invalid('unsafe experiment root')
    scope=launch.reviews_root()/'planar-account-admission-v1'
    if scope.is_symlink() or (scope/'lock').is_symlink() or (scope/'records').is_symlink(): raise schema2.Invalid('unsafe account scope')
    if len(reservation_id)!=32 or any(c not in '0123456789abcdef' for c in reservation_id): raise schema2.Invalid('unsafe execution id')
    if not isinstance(reason,str) or not reason.strip(): raise schema2.Invalid('explicit resolution reason required')
    with codex_run.admission_scope() as records, _controller_lock(root):
        m=schema2.validate(root,identity)
        if m.get('runnable_contract') != schema2_runnable.CONTRACT: raise schema2.Invalid('runnable schema-2 experiment required')
        path=root/'reservations'/f'{reservation_id}.json'; r=schema2.read_record(path); schema2_runnable.closed(r,schema2_runnable.RESERVATION_FIELDS)
        if r['runnable_contract']!=schema2_runnable.CONTRACT or r['experiment_identity']!=identity or r['execution_id']!=reservation_id or r['account_scope_id']!=str(records.parent.resolve()): raise schema2.Invalid('reservation or account scope mismatch')
        gate_path=Path(gate_path); reviews=launch.reviews_root()
        if gate_path.is_symlink() or not gate_path.is_file() or not gate_path.resolve().is_relative_to(reviews.resolve()): raise schema2.Invalid('unsafe gate')
        gate_bytes=gate_path.read_bytes(); gate=pv.parse(gate_bytes,1024*1024); schema2_runnable.closed(gate,schema2_runnable.GATE_FIELDS)
        marker_hash=hashlib.sha256(json.dumps(gate,sort_keys=True,separators=(',',':')).encode()).hexdigest()
        expected_shared={'experiment':str(root.resolve()),'execution_id':reservation_id,'runnable_contract':schema2_runnable.CONTRACT,'reservation_sha256':schema2.h(path.read_bytes()),'gate_marker_sha256':marker_hash,'gate_raw_sha256':schema2.h(gate_bytes)}
        if schema2.read_record(records/f'{reservation_id}.start.json')!=expected_shared: raise schema2.Invalid('shared admission mismatch')
        folder=root/'executions'/reservation_id
        if folder.is_symlink() or not folder.is_dir(): raise schema2.Invalid('unsafe execution')
        start=schema2.read_record(folder/'start.json')
        if start.get('execution_id')!=reservation_id or start.get('reservation_sha256')!=schema2.h(path.read_bytes()) or start.get('gate_marker_sha256')!=marker_hash: raise schema2.Invalid('local start mismatch')
        marker=reviews/'harbor-t044-consumed'/marker_hash
        if marker.parent.is_symlink() or marker.is_symlink() or not marker.is_file() or marker.read_bytes()!=b'reserved\n': raise schema2.Invalid('gate consumption missing')
        terminal=schema2.read_record(folder/'terminal.json')
        if terminal.get('status') not in ('incomplete','error'): raise schema2.Invalid('stopped terminal required')
        if not (folder/'account-stop.json').is_file() or (folder/'account-stop.json').is_symlink(): raise schema2.Invalid('account stop missing')
        dispatch=schema2.read_record(folder/'dispatch.json'); run_id='planar-t044-'+reservation_id; evidence=Path(dispatch['evidence'])
        if dispatch.get('launcher_id')!=run_id or evidence!=launch.jobs_root()/run_id or evidence.is_symlink() or not evidence.is_dir() or (evidence/'recovery-required').exists() or (evidence/'recovery-required').is_symlink(): raise schema2.Invalid('cleanup or dispatch unconfirmed')
        def read_json(name):
            file=evidence/name
            if file.is_symlink() or not file.is_file(): raise schema2.Invalid('cleanup evidence missing')
            with file.open() as stream: return json.load(stream,object_pairs_hook=__import__('schema2_admission').no_duplicates)
        watchdog=read_json('watchdog-terminal.json'); launcher=read_json('launcher-terminal.json')
        if watchdog.get('execution_id')!=run_id or launcher.get('execution_id')!=run_id or watchdog.get('status') not in ('completed','interrupted') or launcher.get('status')!=watchdog.get('status'): raise schema2.Invalid('cleanup status unconfirmed')
        closed=records/f'{reservation_id}.closed.json'; resolved=records/f'{reservation_id}.resolved.json'; local=folder/'account-stop-resolved.json'
        if closed.exists() or closed.is_symlink(): raise schema2.Invalid('account is already closed')
        record={'reason':reason}
        has_local=local.exists() or local.is_symlink(); has_shared=resolved.exists() or resolved.is_symlink()
        if has_local and schema2.read_record(local)!=record: raise schema2.Invalid('conflicting local resolution')
        if has_shared and schema2.read_record(resolved)!=record: raise schema2.Invalid('conflicting shared resolution')
        if not has_local: schema2.write_new(local,record)
        if not has_shared: schema2.write_new(resolved,record)
        return {'execution_id':reservation_id,'status':'resolved'}
