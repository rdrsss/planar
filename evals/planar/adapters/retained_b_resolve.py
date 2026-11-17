"""Explicitly release retained-B account admission without changing its outcome."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import launch
from retained_b_support import verify_runtime


def digest(path):
    path = Path(path)
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 4 * 1024 * 1024:
        raise ValueError('unsafe or missing evidence')
    return hashlib.sha256(path.read_bytes()).hexdigest()


def resolve(evidence, case, candidate, audit):
    """Require an independently authored bound release, terminal cleanup, and exact source/gate."""
    sys.path.insert(0, str(launch.EVALS / 'planar/experiments'))
    try:
        import codex_run
        evidence, case, candidate, audit = map(Path, (evidence, case, candidate, audit))
        if (evidence.is_symlink() or evidence.resolve(strict=True).parent != launch.jobs_root().resolve()
            or not re.fullmatch(r'planar-t044-[0-9a-f]{32}', evidence.name)
            or audit.is_symlink() or not audit.resolve(strict=True).is_relative_to(launch.reviews_root().resolve())
            or audit.resolve(strict=True).is_relative_to(launch.jobs_root().resolve())):
            raise ValueError('unsafe execution or audit path')
        scope = launch.reviews_root()/'planar-account-admission-v1'
        if scope.is_symlink() or (scope/'lock').is_symlink() or (scope/'records').is_symlink():
            raise ValueError('unsafe retained account scope')
        with codex_run.admission_scope() as records:
            admission_path = evidence/'retained-admission.json'
            admission = json.loads(admission_path.read_text())
            gate = json.loads(Path(admission['gate_path']).read_text())
            environment = {'PLANAR_T044_EXPECTED_IMAGE_ID':gate['image_id'],
                           'PLANAR_T045_VERIFIER_IMAGE_ID':gate['verifier_image_id'],
                           'PLANAR_T044_ABSOLUTE_DEADLINE':gate['absolute_deadline']}
            previous = {key:os.environ.get(key) for key in environment}
            try:
                os.environ.update(environment)
                verify_runtime(evidence, case, Path(admission['source_packet_root']), candidate)
            finally:
                for key, value in previous.items():
                    if value is None: os.environ.pop(key,None)
                    else: os.environ[key] = value
            start = records/(evidence.name+'.start.json')
            if start.is_symlink() or not start.is_file() or json.loads(start.read_text()).get('status') != 'awaiting-independent-bound-raw-audit':
                raise ValueError('account start unavailable')
            if (evidence/'recovery-required').exists() or (evidence/'recovery-required').is_symlink():
                raise ValueError('cleanup unconfirmed')
            for name in ('watchdog-terminal.json','launcher-terminal.json'):
                p = evidence/name
                if json.loads(p.read_text()).get('execution_id') != evidence.name or p.is_symlink():
                    raise ValueError('terminal evidence missing')
            statuses = list((evidence/evidence.name).glob('*/verifier/status.json'))
            if len(statuses) != 1:
                raise ValueError('unique verifier status required')
            status = json.loads(statuses[0].read_text())
            reward = statuses[0].with_name('reward.txt')
            if (status.get('status') != 'incomplete' or status.get('file_structure') != 'pass'
                or not isinstance(status.get('structural_checks'),dict) or status.get('semantic_audit') != 'required'
                or status.get('source_packet_sha256') != admission['source_packet_sha256']
                or reward.is_symlink() or not reward.is_file() or reward.read_text().strip() != '0'):
                raise ValueError('only incomplete outcome can be released')
            registry = evidence/'projects'
            projects = list(registry.glob('*.json'))
            if registry.is_symlink() or not registry.is_dir() or not projects or len(projects) != len(list(registry.iterdir())):
                raise ValueError('project registry unavailable')
            for path in projects:
                if path.is_symlink(): raise ValueError('unsafe project record')
                item = json.loads(path.read_text()); project = item.get('compose_project')
                if item.get('execution_id') != evidence.name or not isinstance(project,str) or not re.fullmatch(re.escape(evidence.name.lower())+r'-[a-z0-9_-]+',project):
                    raise ValueError('project identity mismatch')
                for command in (['container','ls','-aq'],['network','ls','-q'],['volume','ls','-q']):
                    result = subprocess.run(['docker',*command,'--filter','label=com.docker.compose.project='+project],capture_output=True,text=True,timeout=15)
                    if result.returncode or result.stdout.strip(): raise ValueError('project cleanup unconfirmed')
            expected = {'execution_id':evidence.name,'decision':'release-account','behavior':'incomplete',
                        'gate_sha256':digest(admission['gate_path']), 'source_packet_sha256':admission['source_packet_sha256'],
                        'verifier_status_sha256':digest(statuses[0]),'reward_sha256':digest(reward)}
            document = json.loads(audit.read_text())
            if not isinstance(document,dict) or any(document.get(k) != v for k,v in expected.items()) or not isinstance(document.get('reason'),str) or not document['reason'].strip():
                raise ValueError('independent release evidence does not bind this outcome')
            result = records/(evidence.name+'.resolved.json')
            if (records/(evidence.name+'.closed.json')).exists(): raise ValueError('account already closed')
            payload={'execution_id':evidence.name,'behavior':'incomplete','audit_sha256':digest(audit),
                     'gate_sha256':expected['gate_sha256'],'source_packet_sha256':expected['source_packet_sha256'],
                     'verifier_status_sha256':expected['verifier_status_sha256'],'reward_sha256':expected['reward_sha256']}
            data=(json.dumps(payload,sort_keys=True)+'\n').encode()
            if result.exists() or result.is_symlink():
                if result.is_symlink() or result.read_bytes()!=data: raise ValueError('conflicting resolution')
                return {'execution_id':evidence.name,'account':'resolved','behavior':'incomplete'}
            fd,temporary=tempfile.mkstemp(prefix='.retained-resolution-',dir=records)
            try:
                with os.fdopen(fd,'wb') as out:
                    out.write(data); out.flush(); os.fsync(out.fileno())
                os.link(temporary,result,follow_symlinks=False)
                directory=os.open(records,os.O_RDONLY|os.O_DIRECTORY)
                try: os.fsync(directory)
                finally: os.close(directory)
            finally:
                os.unlink(temporary)
            return {'execution_id':evidence.name,'account':'resolved','behavior':'incomplete'}
    finally:
        sys.path.remove(str(launch.EVALS / 'planar/experiments'))


if __name__ == '__main__':
    if len(sys.argv) != 5: raise SystemExit('usage: retained_b_resolve.py EVIDENCE CASE CANDIDATE INDEPENDENT_RELEASE_JSON')
    print(json.dumps(resolve(*sys.argv[1:]),sort_keys=True))
