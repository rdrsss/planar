"""Versioned A-only cached regrade; no agent, auth, or mutation of evidence."""
from pathlib import Path
import subprocess
import sys

IMAGE = 'sha256:0ca35a67f73d3f023852541e9de76218ab40d692c4fa5c40bb85b094eca4c800'
ORACLE = 't045-a-linked-observations-v6'

def main():
    if len(sys.argv) != 2: raise SystemExit('usage: regrade_retained_a.py EVIDENCE_ROOT')
    root = Path(sys.argv[1]).resolve(strict=True)
    if not root.is_dir(): raise ValueError('evidence root is not a directory')
    command = ['docker','run','--rm','--network','none','--read-only','--cap-drop','ALL',
               '--security-opt','no-new-privileges','--mount',f'type=bind,src={root},dst=/evidence,readonly',
               IMAGE,'python3','-c',
               'import json,sys; sys.path.insert(0,"/opt/planar-clean"); '
               'from verify_a import assess; print(json.dumps(assess("/evidence"),sort_keys=True))']
    print('oracle='+ORACLE, flush=True)
    print('image='+IMAGE, flush=True)
    return subprocess.run(command,timeout=60,check=False).returncode

if __name__ == '__main__': raise SystemExit(main())
