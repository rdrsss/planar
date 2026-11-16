"""Run two retained, model-free Harbor recovery lifecycle controls."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid
from launch import ROOT, REVIEWS, CASES, config_for


def run(variant):
    name = 'planar-t044-noauth-' + uuid.uuid4().hex
    evidence = REVIEWS/'harbor-t044-jobs'/name
    evidence.mkdir(parents=True, mode=0o700)
    config = config_for(name, evidence, CASES['fresh-interrupted-review'])
    if variant not in ('positive', 'changed-check', 'recorded-live'): raise ValueError('unknown control variant')
    config['agents'][0]['import_path'] = ('recovery_control:RecordedLiveRecoveryControl' if variant == 'recorded-live' else 'recovery_control:SyntheticRecoveryControl')
    config['agents'][0]['kwargs']['skip_install'] = True
    config_path = evidence/'control-config.json'
    config_path.write_text(json.dumps(config, indent=2)+'\n')
    docker = shutil.which('docker')
    if not docker: raise RuntimeError('docker unavailable')
    env = {'HOME':str(Path.home()), 'PATH':os.pathsep.join([str(Path(docker).resolve().parent),str(Path(sys.executable).parent),'/usr/bin','/bin','/usr/sbin','/sbin']),
           'PYTHONPATH':str(ROOT/'evals/planar/adapters'), 'PLANAR_T044_EVIDENCE_DIR':str(evidence),
           'PLANAR_T044_EXPECTED_IMAGE_ID':'sha256:06e95472c9fb2a7ab49c50b0f2c87f5f1006aa33397ef17715a75164458b7ed2',
           'PLANAR_T044_CASE':'fresh-interrupted-review', 'PLANAR_T045_CONTROL_VARIANT':variant,
           'OPENAI_API_KEY':'', 'CODEX_API_KEY':''}
    try:
        with (evidence/'harbor-control.log').open('w') as log:
            result = subprocess.run([str(Path(sys.executable).parent/'harbor'),'run','--config',str(config_path),'--yes'], env=env, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=180)
    except subprocess.TimeoutExpired:
        print(json.dumps({'variant':variant,'status':'timeout_cleanup_unproven','evidence':str(evidence)}), flush=True)
        raise
    trials = list((evidence/name).glob('fresh-interrupted-review__*/result.json'))
    if len(trials) != 1: raise RuntimeError('trial result missing; evidence '+str(evidence))
    trial = trials[0].parent
    trial_result = json.loads(trials[0].read_text())
    status = json.loads((trial/'verifier/status.json').read_text())
    reward = (trial/'verifier/reward.txt').read_text().strip()
    network = json.loads((evidence/'verifier-network.json').read_text())
    projects = [json.loads(p.read_text())['compose_project'] for p in (evidence/'projects').glob('*.json')]
    if len(projects) != 2: raise RuntimeError('project registration missing; evidence '+str(evidence))
    checks = {}
    for project in projects:
        p = subprocess.run(['docker','ps','-aq','--no-trunc','--filter','label=com.docker.compose.project='+project],capture_output=True,text=True,check=True,timeout=15)
        checks[project] = p.stdout.splitlines()
    container = json.loads((evidence/'planar-container.json').read_text())['id']
    for cid in (container, network['container_id']):
        checks['container:'+cid] = subprocess.run(['docker','ps','-aq','--no-trunc','--filter','id='+cid],capture_output=True,text=True,check=True,timeout=15).stdout.splitlines()
    artifacts = trial/'artifacts'
    files = [{'path':p.relative_to(artifacts).as_posix(), 'sha256':hashlib.sha256(p.read_bytes()).hexdigest(), 'size':p.stat().st_size}
             for p in sorted(artifacts.rglob('*')) if p.is_file()]
    receipt = {'synthetic':variant != 'recorded-live', 'recorded_live':variant == 'recorded-live', 'model_invoked':False, 'variant':variant, 'project_and_container_checks':checks,
               'cleanup_confirmed':all(not x for x in checks.values()), 'artifact_inventory':files,
               'verifier_network':network, 'status':status, 'reward':reward, 'harbor_exit':result.returncode,
               'trial_exception':trial_result.get('exception_info')}
    (evidence/'synthetic-control-audit.json').write_text(json.dumps(receipt,indent=2,sort_keys=True)+'\n')
    expected = ('pass','1') if variant in ('positive','recorded-live') else ('behavioral_failure','0')
    print(json.dumps({'variant':variant,'evidence':str(evidence),'status':status.get('status'),'reward':reward,
                      'cleanup_confirmed':receipt['cleanup_confirmed'],'artifacts':len(files)}),flush=True)
    if (result.returncode != 0 or trial_result.get('exception_info') is not None or
        network.get('network_mode') != 'none' or not receipt['cleanup_confirmed'] or
        (variant != 'recorded-live' and 'home/projects/revision-fault/tasks/T-001/custom-preflight-note.md' not in status.get('unassessed_extra_evidence', [])) or
        (status.get('status'),reward) != expected):
        raise RuntimeError('control failed; retained evidence '+str(evidence))


if __name__ == '__main__':
    run('positive')
    run('changed-check')
