"""Run the cached narrow regrade in a network-disabled verifier image."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from regrade_dispatch import ROOT, CANDIDATE, MANIFEST
from candidate import inventory

IMAGE = 'sha256:009b99753fae02f56741df7ba9113d7cbe5c3069a435814f003f4261ce582b84'
HOST_FILES = ('launcher.json','job-config.json','planar-image.json','planar-cli.json',
              'planar-install-provenance.json','planar-launch.json','planar-terminal.json',
              'planar-cleanup.json','launcher-terminal.json','planar-container.json','planar-preauth.json')


def copy_file(source, destination):
    if source.is_symlink() or not source.is_file():
        raise ValueError('missing or unsafe selected evidence')
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def main():
    evidence = Path(sys.argv[1]).resolve(strict=True)
    if inventory(CANDIDATE) != json.loads(MANIFEST.read_text()):
        raise ValueError('candidate differs from manifest')
    trials=list((evidence/evidence.name).glob('plugin-smoke__*/result.json'))
    if len(trials)!=1: raise ValueError('expected one trial')
    trial=trials[0].parent
    rollouts=list((trial/'agent/sessions').rglob('rollout-*.jsonl'))
    if len(rollouts)!=2 or any(any(p.is_symlink() for p in [item,*list(item.parents)[:4]]) for item in rollouts):
        raise ValueError('unsafe or incomplete rollouts')
    selected=[trial/'result.json',trial/'artifacts/workspace/RESULT.txt',*rollouts]
    with tempfile.TemporaryDirectory(prefix='t044-regrade-',dir=ROOT.parent) as tmp:
        bundle=Path(tmp)
        shutil.copytree(CANDIDATE,bundle/'plugins/planar',symlinks=False)
        copy_file(MANIFEST,bundle/'evals/planar/cases/plugin-smoke/candidate.json')
        for name in ('candidate.py','check_provenance.py','regrade_dispatch.py'):
            copy_file(Path(__file__).with_name(name),bundle/'evals/planar/adapters'/name)
        for name in HOST_FILES:
            copy_file(evidence/name,bundle/'evidence'/evidence.name/name)
        for path in selected:
            relative = path.relative_to(evidence)
            current = evidence
            for part in relative.parts[:-1]:
                current = current / part
                if current.is_symlink():
                    raise ValueError('symlink in selected evidence path')
            copy_file(path,bundle/'evidence'/evidence.name/relative)
        command=['docker','run','--rm','--network','none','--read-only','--cap-drop','ALL','--security-opt','no-new-privileges','-v',f'{bundle}:/bundle:ro',IMAGE,'python3','/bundle/evals/planar/adapters/regrade_dispatch.py','/bundle/evidence/'+evidence.name]
        result=subprocess.run(command,text=True,capture_output=True,timeout=60)
        print(result.stdout,end='')
        if result.returncode:
            print('isolated regrade failed',file=sys.stderr)
        return result.returncode

if __name__ == '__main__': sys.exit(main())
