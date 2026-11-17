"""Fresh locked consumer check for the exact resolved source account scope."""
import json
import os
from pathlib import Path
import subprocess
import sys


def _validate(packet,candidate):
    import importlib.util
    path=Path(__file__).with_name('source_packet.py')
    if path.is_symlink() or not path.is_file(): raise ValueError('unsafe source validator')
    spec=importlib.util.spec_from_file_location('_clean_retained_source_packet',path)
    module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module.validate(packet,candidate)


def check(packet, candidate, project_reviews):
    """Call the existing consumer in the original scope, without accessing auth."""
    packet=Path(packet); _validate(packet,candidate)
    reviews=Path(project_reviews).resolve(strict=True)
    gate=json.loads((packet/'evidence/original-gate.json').read_bytes())
    scope=Path(gate.get('account_scope_id',''))
    if not scope.is_absolute() or '..' in scope.parts or scope.name!='planar-account-admission-v1':
        raise ValueError('source account scope invalid')
    root=scope.parent.resolve(strict=True)
    if not root.is_relative_to(reviews) or scope.resolve(strict=True)!=root/'planar-account-admission-v1':
        raise ValueError('source account scope outside reviews')
    import launch
    worktree=launch.ROOT
    env={'PATH':os.environ.get('PATH',os.defpath), 'HOME':str(Path.home()),
         'PYTHONPATH':os.pathsep.join(str(worktree/p) for p in ('evals/planar/adapters','evals/planar/experiments')),
         'PLANAR_EVALS_OUTPUT_ROOT':str(root)}
    marker=packet/'evidence/original-resolution.json'
    import hashlib
    marker_hash=hashlib.sha256(marker.read_bytes()).hexdigest()
    eid=gate.get('execution_id')
    if not isinstance(eid,str) or not eid.startswith('planar-t044-') or '/' in eid:
        raise ValueError('source execution identity invalid')
    program = """import hashlib,json,sys
import clean_work_release,codex_run
with codex_run.admission_scope() as records:
    eid,expected=sys.argv[1:]
    start=records/(eid+'.start.json'); marker=records/(eid+'.resolved.json')
    raw=marker.read_bytes()
    if hashlib.sha256(raw).hexdigest()!=expected: raise ValueError('source resolution changed')
    if not clean_work_release.validate_published(start,marker,json.loads(raw)):
        raise ValueError('source account resolution invalid')
    print(json.dumps(codex_run.shared_pending(records)))
"""
    result=subprocess.run([sys.executable,'-B','-c',program,eid,marker_hash],env=env,capture_output=True,text=True,check=True,timeout=90)
    if json.loads(result.stdout)!=[]: raise ValueError('source account has pending records')
    return {'source_account':'resolved-and-clean','scope':str(scope),'scope_limit':'selected source account only'}
