"""Case-specific no-fault retained-work host setup; not a launcher or gate."""
from pathlib import Path
import json
import os
import time
import shlex
import shutil
import subprocess
import tempfile


async def prepare_target(environment, case, packet, candidate, evidence, source_job, target_job, expected_digest):
    """Restore only pinned durable data and capture a fresh no-fault handoff.

    Caller must first own reviewed gate and account admission. This helper is
    unregistered until that wiring and the separate verifier are reviewed.
    """
    from clean_revision_support import load_case_module, check_revision_time
    case,packet,candidate,evidence=map(Path,(case,packet,candidate,evidence))
    policy=json.loads((case/'source-policy.json').read_bytes())
    if source_job!=policy.get('job') or not isinstance(target_job,str) or not target_job or target_job==source_job:
        raise ValueError('source and target job identities invalid')
    native_path=evidence/'native-trial-start.json'
    if native_path.is_symlink() or not native_path.is_file(): raise ValueError('target native start unavailable')
    native=json.loads(native_path.read_bytes())
    if native.get('job_id')!=target_job or Path(native.get('job_dir',''))!=evidence/evidence.name:
        raise ValueError('target native job mismatch')
    source=load_case_module(case,'source_packet')
    if source.validate(packet,candidate)['source_packet_sha256'] != expected_digest:
        raise ValueError('source packet mismatch')
    import launch
    account=load_case_module(case,'source_account')
    account.check(packet,candidate,launch.PROJECT/'reviews')
    target=evidence/'retained-clean'
    if target.exists() or target.is_symlink(): raise ValueError('target boundary exists')
    with tempfile.TemporaryDirectory(prefix='retained-clean-',dir=evidence) as tmp:
        root=Path(tmp); root.chmod(0o755); stage=root/'case'; stage.mkdir()
        for name in ('materialize.py','install_fixture.py','projection.json','durable-policy.json'):
            shutil.copyfile(case/name,stage/name)
        shutil.copytree(case/'fixture',stage/'fixture')
        from retained_b_support import _run
        proof=_run(case,'materialize',(packet,candidate,stage/'durable'))
        if proof['source_packet_sha256']!=expected_digest: raise ValueError('durable packet drift')
        await environment.upload_dir(root,'/tmp/planar-retained-clean-stage')
    program=('import json,sys; sys.path.insert(0,"/tmp/planar-retained-clean-stage/case"); '
             'from materialize import install; print(json.dumps(install("/tmp/planar-retained-clean-stage/case/durable","/workspace",sys.argv[1])))')
    result=await environment.exec('python3 -c '+shlex.quote(program)+' '+shlex.quote(expected_digest),timeout_sec=120)
    if result.return_code!=0: raise ValueError('durable install failed')
    installed=json.loads(result.stdout)
    if installed.get('source_packet_sha256')!=expected_digest or installed.get('status')!='restored':
        raise ValueError('durable install identity mismatch')
    cleanup=await environment.exec('rm -rf /tmp/planar-retained-clean-stage && test ! -e /tmp/planar-retained-clean-stage',timeout_sec=30)
    if cleanup.return_code!=0: raise ValueError('temporary projection cleanup failed')
    capture=load_case_module(case,'capture'); boundary=load_case_module(case,'no_fault')
    short=await environment._platform._resolve_service_container('main')
    cid=subprocess.run(['docker','inspect','--format','{{.Id}}',short],check=True,capture_output=True,text=True,timeout=15).stdout.strip()
    baseline=capture.census(cid); target.mkdir(mode=0o700)
    (target/'container.json').write_text(json.dumps({'container_id':cid,'baseline':baseline})+'\n')
    (target/'target-provenance.json').write_text(json.dumps({'execution_id':evidence.name,'job_id':target_job},sort_keys=True)+'\n')
    original=capture.load(packet/'source/pre')
    before=await capture.collect(environment,cid,baseline,failure_evidence=target)
    capture.save(target/'restored-pre',before)
    # Reserve the entire 400-second agent, 60-second verifier, 15-second grace
    # and 90 seconds for remaining host handoff, not a second model call.
    check_revision_time(evidence,565)
    source_receipt=json.loads((packet/'source/pre-receipt.json').read_bytes())
    outcome=await boundary.handoff(environment,capture,container_id=cid,baseline=baseline,
        source=original,target=target,source_receipt=source_receipt,source_job=source_job,target_job=target_job)
    if source.validate(packet,candidate)['source_packet_sha256'] != expected_digest:
        raise ValueError('source packet changed during restore')
    receipt={'source_packet_sha256':expected_digest,'source_job':source_job,'target_job':target_job,
             'boundary':outcome['boundary'],'wall_ns':time.time_ns(),'monotonic_ns':time.monotonic_ns()}
    fd=os.open(target/'no-fault.json',os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    with os.fdopen(fd,'w') as stream: json.dump(receipt,stream,sort_keys=True); stream.write('\n')
    return {'source_packet_sha256':expected_digest,'container_id':cid,'status':'restored-no-fault',
            'boundary':outcome['boundary']}


async def verifier_handoff(environment, case, packet, candidate, evidence, expected_digest, destination):
    """Export a host-only copy to a separate verifier after target termination."""
    from candidate import inventory
    from clean_revision_support import load_case_module
    from safe_export import write_tree
    case,packet,candidate,evidence,destination=map(Path,(case,packet,candidate,evidence,destination))
    source=load_case_module(case,'source_packet')
    if source.validate(packet,candidate)['source_packet_sha256'] != expected_digest:
        raise ValueError('source packet mismatch before verifier handoff')
    expected=json.loads((case/'candidate.json').read_text())
    if inventory(candidate)!=expected: raise ValueError('candidate changed before verifier handoff')
    capture=load_case_module(case,'capture'); target=evidence/'retained-clean'
    identity=json.loads((target/'container.json').read_text())
    short=await environment._platform._resolve_service_container('main')
    actual=subprocess.run(['docker','inspect','--format','{{.Id}}',short],check=True,capture_output=True,text=True,timeout=15).stdout.strip()
    if actual!=identity.get('container_id'): raise ValueError('target container changed')
    if not (target/'logs/revision').is_dir(): raise ValueError('target native logs missing')
    final=await capture.collect(environment,actual,identity.get('baseline'),failure_evidence=target)
    capture.save(target/'final',final)
    home={name[5:]:value for name,value in final.items() if name.startswith('home/')}
    write_tree(destination,home)
    host=destination/'_planar_host'
    if host.exists() or host.is_symlink(): raise ValueError('host evidence destination exists')
    host.mkdir(mode=0o700)
    shutil.copytree(packet,host/'source-packet',symlinks=True)
    shutil.copytree(candidate,host/'candidate',symlinks=True)
    shutil.copytree(target,host/'target',symlinks=True,ignore=shutil.ignore_patterns('rejected-home.tar','rejected-home.json'))
    for item in (host,*host.rglob('*')):
        if item.is_symlink(): raise ValueError('symlink in verifier input')
        if item.is_dir(): item.chmod(0o755)
        elif item.is_file(): item.chmod(0o644)
        else: raise ValueError('special verifier input')
    return {'status':'verifier-copy-ready','source_packet_sha256':expected_digest}


def verify_runtime(evidence, case, packet, candidate):
    """Recheck host-owned one-use admission and frozen inputs, without auth reads."""
    import hashlib
    import launch
    from clean_revision_support import load_case_module
    from candidate import inventory
    evidence,case,packet,candidate=map(Path,(evidence,case,packet,candidate))
    path=evidence/'retained-admission.json'
    if path.is_symlink() or not path.is_file(): raise ValueError('retained host admission missing')
    record=json.loads(path.read_text())
    if record.get('execution_id')!=evidence.name: raise ValueError('retained execution identity changed')
    gate_path=Path(record.get('gate_path',''))
    if gate_path.is_symlink() or not gate_path.is_file() or not gate_path.resolve().is_relative_to(launch.reviews_root().resolve()):
        raise ValueError('retained gate path unsafe')
    raw=gate_path.read_bytes(); gate=json.loads(raw)
    if hashlib.sha256(raw).hexdigest()!=record.get('gate_raw_sha256'): raise ValueError('retained gate drift')
    canonical=hashlib.sha256(json.dumps(gate,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    marker=launch.reviews_root()/'harbor-t044-consumed'/canonical
    if marker.is_symlink() or not marker.is_file() or marker.read_text()!='reserved\n': raise ValueError('retained gate not consumed')
    if gate != record.get('gate_authority'): raise ValueError('retained admission authority mismatch')
    if (gate.get('execution_id')!=evidence.name or gate.get('source_packet_sha256')!=record.get('source_packet_sha256') or
        gate.get('case_sha256')!=record.get('case_sha256') or gate.get('candidate_sha256')!=record.get('candidate_sha256') or
        gate.get('effective_configuration_sha256')!=record.get('effective_configuration_sha256') or
        gate.get('verifier_image_id')!=record.get('verifier_image_id') or
        gate.get('verifier_image_id')!=os.environ.get('PLANAR_T045_VERIFIER_IMAGE_ID') or
        gate.get('image_id')!=os.environ.get('PLANAR_T044_EXPECTED_IMAGE_ID') or
        gate.get('absolute_deadline')!=os.environ.get('PLANAR_T044_ABSOLUTE_DEADLINE') or gate.get('verdict')!='approve'):
        raise ValueError('retained runtime gate binding mismatch')
    if str(packet.resolve(strict=True))!=record.get('source_packet_root') or gate.get('source_packet_root')!=record.get('source_packet_root'):
        raise ValueError('retained source packet path changed')
    scope=Path(record.get('account_scope_id',''))
    if scope!=launch.reviews_root().resolve()/'planar-account-admission-v1' or gate.get('account_scope_id')!=str(scope):
        raise ValueError('retained account scope changed')
    account=scope/'records'/(evidence.name+'.start.json')
    if account.is_symlink() or not account.is_file(): raise ValueError('retained account admission missing')
    shared=json.loads(account.read_text())
    if shared != {'execution_id':evidence.name,'case':'clean-retained-a-continuation','gate_sha256':record['gate_raw_sha256'],
                   'gate_authority':gate,'status':'awaiting-independent-bound-raw-audit'}:
        raise ValueError('retained shared authority mismatch')
    launcher_path=evidence/'launcher.json'
    if launcher_path.is_symlink() or not launcher_path.is_file(): raise ValueError('retained launcher authority missing')
    launcher=json.loads(launcher_path.read_text())
    for key in ('execution_id','case','case_sha256','candidate_sha256','implementation_sha256','configuration_sha256','image_id'):
        if launcher.get(key) != (evidence.name if key == 'execution_id' else gate.get(key)):
            raise ValueError('retained launcher authority mismatch')
    if launcher.get('review_sha256') != record['gate_raw_sha256'] or launch.source_digest(launch.EVALS/'planar/cases/clean-retained-a-continuation') != gate.get('implementation_sha256'):
        raise ValueError('retained implementation authority mismatch')
    if os.environ.get('CODEX_AUTH_JSON_PATH') is not None and os.environ['CODEX_AUTH_JSON_PATH'] != gate.get('auth_path'):
        raise ValueError('retained auth authority mismatch')
    if launch.case_inventory(case)!=record['case_sha256'] or inventory(candidate)['sha256']!=record['candidate_sha256']:
        raise ValueError('retained frozen case or candidate changed')
    config_path=evidence/'job-config.json'
    if config_path.is_symlink() or not config_path.is_file(): raise ValueError('retained effective config missing')
    config=json.loads(config_path.read_text())
    actual=hashlib.sha256(json.dumps(config,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    if actual!=record['effective_configuration_sha256']: raise ValueError('retained effective config changed')
    if load_case_module(case,'source_packet').validate(packet,candidate).get('source_packet_sha256')!=record['source_packet_sha256']:
        raise ValueError('retained source packet changed')
    return record
