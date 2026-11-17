"""Case-specific no-auth host helpers for the retained-A/fresh-B experiment."""
from pathlib import Path
import json
import os
import subprocess
import sys

PROJECT=Path(__file__).resolve().parents[3].parent
CASE_NAME='retained-a-fresh-b'


def _run(case, name, arguments, timeout=90):
    case=Path(case)
    if case.is_symlink() or not case.is_dir(): raise ValueError('unsafe retained case')
    env={'PATH':os.environ.get('PATH',os.defpath),'HOME':str(Path.home()),
         'PYTHONPATH':os.pathsep.join((str(case),str(Path(__file__).resolve().parent)))}
    program=('import json,sys; from '+name+' import '+('validate' if name=='source_packet' else 'prepare')+' as f; '
             'print(json.dumps(f(*sys.argv[1:]),sort_keys=True))')
    result=subprocess.run([sys.executable,'-B','-c',program,*map(str,arguments)],env=env,capture_output=True,text=True,check=True,timeout=timeout)
    value=json.loads(result.stdout)
    if not isinstance(value,dict): raise ValueError('invalid retained source result')
    return value


def source(case, packet, candidate):
    raw=Path(packet)
    if raw.is_symlink() or not raw.is_dir(): raise ValueError('unsafe retained source packet')
    packet=raw.resolve(strict=True)
    if not packet.is_relative_to((PROJECT/'reviews').resolve(strict=True)):
        raise ValueError('retained source packet outside project reviews')
    return _run(case,'source_packet',(packet,candidate))


def prepare(case, packet, candidate, destination):
    source(case,packet,candidate)
    return _run(case,'materialize',(packet,candidate,destination))


async def prepare_target(environment, case, packet, candidate, evidence, expected_digest):
    """No-model target setup at a quiescent native agent boundary.

    Caller owns gate/admission and the target environment. No source packet or
    source native log is uploaded to the target; only the fixed durable subset.
    """
    import shlex
    import shutil
    import tempfile
    import time
    from clean_revision_support import load_case_module, check_revision_time

    case,packet,candidate,evidence=map(Path,(case,packet,candidate,evidence))
    if source(case,packet,candidate).get('source_packet_sha256')!=expected_digest:
        raise ValueError('source packet changed before target restoration')
    target=evidence/'retained-b'
    if target.exists() or target.is_symlink(): raise ValueError('retained target already exists')
    with tempfile.TemporaryDirectory(prefix='retained-b-',dir=evidence) as tmp:
        root=Path(tmp); root.chmod(0o755); stage=root/'retained'; stage.mkdir()
        for name in ('materialize.py','install_fixture.py','projection.json','durable-policy.json'):
            shutil.copyfile(case/name,stage/name)
        shutil.copytree(case/'fixture',stage/'fixture')
        proof=prepare(case,packet,candidate,stage/'durable')
        if proof.get('source_packet_sha256')!=expected_digest: raise ValueError('durable projection binding changed')
        await environment.upload_dir(root,'/tmp/planar-retained-stage')
    program=('import json,sys; sys.path.insert(0,"/tmp/planar-retained-stage/retained"); '
             'from materialize import install; print(json.dumps(install("/tmp/planar-retained-stage/retained/durable","/workspace",sys.argv[1])))')
    result=await environment.exec('python3 -c '+shlex.quote(program)+' '+shlex.quote(expected_digest),timeout_sec=120)
    if result.return_code!=0: raise ValueError('retained durable target install failed')
    installed=json.loads(result.stdout)
    if installed.get('source_packet_sha256')!=expected_digest or installed.get('status')!='restored':
        raise ValueError('retained target installation mismatch')
    cleanup=await environment.exec('rm -rf /tmp/planar-retained-stage && test ! -e /tmp/planar-retained-stage',timeout_sec=30)
    if cleanup.return_code!=0: raise ValueError('retained temporary projection cleanup failed')
    capture=load_case_module(case,'capture')
    short=await environment._platform._resolve_service_container('main')
    cid=subprocess.run(['docker','inspect','--format','{{.Id}}',short],check=True,capture_output=True,text=True,timeout=15).stdout.strip()
    baseline=capture.census(cid); target.mkdir(mode=0o700)
    (target/'container.json').write_text(json.dumps({'container_id':cid,'baseline':baseline})+'\n')
    (target/'source.json').write_text(json.dumps({'source_packet_sha256':expected_digest,'source_online_status':'behavioral_failure','source_online_reward':0})+'\n')
    before=await capture.collect(environment,cid,baseline,failure_evidence=target)
    original=capture.load(packet/'source/pre')
    durable={k for k in original if k.startswith('home/') or k in ('repository/README.md','repository/RESULT.txt','repository/STATE.txt')}
    actual_durable={k for k in before if k.startswith('home/') or k in ('repository/README.md','repository/RESULT.txt','repository/STATE.txt')}
    if durable != actual_durable or any(original[k]!=before[k] for k in durable):
        raise ValueError('target restored durable bytes differ from source')
    if any(original.get(k)!=before.get(k) for k in ('host/git-head.txt','host/git-stage.txt','host/git-status.txt')):
        raise ValueError('target restored Git boundary differs from source')
    capture.save(target/'restored-pre',before)
    if source(case,packet,candidate).get('source_packet_sha256')!=expected_digest:
        raise ValueError('source packet changed before injection')
    check_revision_time(evidence,565)
    started={'wall_ns':time.time_ns(),'monotonic_ns':time.monotonic_ns()}
    after,change=await capture.inject(environment,cid,baseline,before,failure_evidence=target)
    ended={'wall_ns':time.time_ns(),'monotonic_ns':time.monotonic_ns()}
    capture.save(target/'post',after)
    (target/'injection.json').write_text(json.dumps({'change':change,'started':started,'ended':ended},sort_keys=True)+'\n')
    return {'source_packet_sha256':expected_digest,'container_id':cid,'status':'restored-and-injected'}


def validate_gate(gate, snapshot, policy, execution_id, absolute_deadline, auth_path, account_scope, verifier_image, *, now=None):
    """Compare a retained-only gate without resolving or reading auth_path."""
    from datetime import datetime, timezone, timedelta
    import re
    if not isinstance(gate,dict) or not re.fullmatch(r'planar-t044-[0-9a-f]{32}',execution_id or ''):
        raise ValueError('retained gate execution identity invalid')
    prereq=snapshot['prerequisites']
    expected={'verdict':'approve','purpose':'one-diagnostic-attempt','case':'retained-a-fresh-b','max_attempts':1,
        'execution_id':execution_id,'absolute_deadline':absolute_deadline,'candidate_sha256':snapshot['candidate_sha256'],
        'case_sha256':snapshot['case_sha256'],'implementation_sha256':snapshot['implementation_sha256'],
        'configuration_sha256':snapshot['configuration_sha256'],'effective_configuration_sha256':prereq['effective_configuration_sha256'],
        'source_packet_sha256':prereq['source_packet_sha256'],'source_packet_root':prereq['source_packet_root'],'source_execution_id':policy['execution'],
        'source_online_status':'behavioral_failure','source_online_reward':0,'source_assessor_image':policy['assessor_image'],
        'source_assessment_sha256':policy['files']['evidence/corrected-assessment.txt']['sha256'],
        'source_semantic_audit_sha256':policy['files']['evidence/semantic-audit.md']['sha256'],
        'image_id':snapshot['image_id'],'verifier_image_id':verifier_image,'account_scope_id':str(account_scope),
        'runtime_budget_seconds':1050,'step_agent_seconds':400,'step_verifier_seconds':60,'cleanup_grace_seconds':15,
        'result_boundary':'incomplete-pending-independent-bound-raw-audit'}
    if any(type(gate.get(k)) is not type(v) or gate.get(k)!=v for k,v in expected.items()):
        raise ValueError('retained gate binding mismatch')
    raw=str(auth_path)
    if not os.path.isabs(raw) or os.path.normpath(raw)!=raw or gate.get('auth_path')!=raw:
        raise ValueError('retained auth path binding mismatch')
    try:
        expiry=datetime.fromisoformat(absolute_deadline)
        current=datetime.now(timezone.utc) if now is None else now
        if expiry.tzinfo is None or current.tzinfo is None or expiry-current < timedelta(seconds=1050): raise ValueError()
    except (TypeError,ValueError,OverflowError):
        raise ValueError('retained gate expired or insufficient remaining time') from None
    return {'execution_id':execution_id,'source_packet_sha256':prereq['source_packet_sha256'],'expires_at':absolute_deadline}


def verify_runtime(evidence, case, packet, candidate):
    """Recheck host-owned one-use admission and frozen inputs, without auth reads."""
    import hashlib
    import launch
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
    if shared != {'execution_id':evidence.name,'case':'retained-a-fresh-b','gate_sha256':record['gate_raw_sha256'],
                   'gate_authority':gate,'status':'awaiting-independent-bound-raw-audit'}:
        raise ValueError('retained shared authority mismatch')
    launcher_path=evidence/'launcher.json'
    if launcher_path.is_symlink() or not launcher_path.is_file(): raise ValueError('retained launcher authority missing')
    launcher=json.loads(launcher_path.read_text())
    for key in ('execution_id','case','case_sha256','candidate_sha256','implementation_sha256','configuration_sha256','image_id'):
        if launcher.get(key) != (evidence.name if key == 'execution_id' else gate.get(key)):
            raise ValueError('retained launcher authority mismatch')
    if launcher.get('review_sha256') != record['gate_raw_sha256'] or launch.source_digest(launch.EVALS/'planar/cases/retained-a-fresh-b') != gate.get('implementation_sha256'):
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
    if source(case,packet,candidate).get('source_packet_sha256')!=record['source_packet_sha256']:
        raise ValueError('retained source packet changed')
    return record
