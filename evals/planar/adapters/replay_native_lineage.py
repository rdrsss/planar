"""Replay retained trials as new lineage assessments; no network or model."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
from candidate import inventory
from check_provenance import check

ROOT = Path(__file__).resolve().parents[3]
CASE = ROOT / 'evals/planar/cases/native-lineage-smoke'
spec = importlib.util.spec_from_file_location('lineage_verify_replay',CASE/'tests/verify.py')
verify = importlib.util.module_from_spec(spec);spec.loader.exec_module(verify)


def digest(root):
    rows=[]
    for path in sorted(root.rglob('*')):
        if '__pycache__' in path.parts: continue
        if path.is_symlink(): raise ValueError('unsafe case path')
        if path.is_file(): rows.append({'path':path.relative_to(root).as_posix(),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
    return hashlib.sha256(json.dumps(rows,ensure_ascii=True,sort_keys=True,separators=(',',':')).encode()).hexdigest()


def replay(evidence):
    evidence=evidence.resolve(strict=True)
    trials=[p for name in ('plugin-smoke','native-lineage-smoke') for p in (evidence/evidence.name).glob(name+'__*/result.json')]
    if len(trials)!=1: raise ValueError('expected one retained original trial')
    trial=trials[0].parent
    for p in (trial,trial/'artifacts',trial/'artifacts/workspace',trial/'agent',trial/'agent/sessions'):
        if p.is_symlink(): raise ValueError('unsafe evidence path')
    check(evidence)
    manifest=json.loads((CASE/'candidate.json').read_text())
    if inventory(ROOT/'plugins/planar')!=manifest: raise ValueError('current candidate mismatch')
    launch=json.loads((evidence/'launcher.json').read_text());install=json.loads((evidence/'planar-install-provenance.json').read_text())
    if launch.get('candidate_sha256')!=manifest['sha256'] or install.get('candidate_sha256')!=manifest['sha256']: raise ValueError('recorded candidate mismatch')
    workspace_preflight=evidence/'planar-workspace-preflight.json'
    workspace_status='unavailable'
    if workspace_preflight.exists() or workspace_preflight.is_symlink():
        if workspace_preflight.is_symlink() or not workspace_preflight.is_file(): raise ValueError('unsafe workspace preflight')
        record=json.loads(workspace_preflight.read_text())
        if record.get('execution_id')!=evidence.name or record.get('status')!='passed' or record.get('fixture')!='/workspace' or record.get('check')!='empty-directory-before-model': raise ValueError('invalid workspace preflight record')
        workspace_status='host-record-cross-checked'
    result=verify.assess(trial/'artifacts/workspace',trial/'agent')
    if result['status']=='pass' and workspace_status=='unavailable':
        result['recorded_lineage_status']='pass'
        result['status']='incomplete'
        result['reason']='host pre-execution workspace evidence unavailable'
    result.update({'assessment':'cached-lineage-v1','source_execution_id':evidence.name,'case_sha256':digest(CASE),'checker_sha256':digest(CASE/'tests'),'candidate_provenance':'host-records-cross-checked','pre_execution_workspace':workspace_status,'new_live_execution':False})
    return result

def main():
    try: result=replay(Path(sys.argv[1]))
    except (IndexError,OSError,ValueError,TypeError,KeyError,json.JSONDecodeError) as exc: result=verify.outcome('infrastructure_error',str(exc),assessment='cached-lineage-v1',new_live_execution=False)
    print(json.dumps(result,indent=2,sort_keys=True))
    return {'pass':0,'behavioral_failure':1}.get(result['status'],2)
if __name__=='__main__':sys.exit(main())
