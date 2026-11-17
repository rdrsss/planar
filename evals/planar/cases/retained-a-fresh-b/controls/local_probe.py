"""Local no-model control only; NOT a Harbor verifier handoff."""
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import time

from restore import restore
from capture import load, save
from snapshot import select_repository
from boundary import verify_injection
from verify_restore import verify


def main(packet, candidate, scratch):
    packet, candidate, scratch = map(Path,(packet,candidate,scratch))
    with tempfile.TemporaryDirectory(prefix='local-retained-',dir=scratch) as tmp:
        root=Path(tmp); workspace=root/'workspace'; workspace.mkdir(); target=root/'target'; target.mkdir()
        proof=restore(packet,candidate,workspace); repo=workspace/'repository'
        source=load(packet/'source/pre')
        env={'PATH':os.defpath,'HOME':'/nonexistent','GIT_CONFIG_NOSYSTEM':'1','GIT_CONFIG_GLOBAL':os.devnull}
        def git(*args):
            return subprocess.run(['git','-c','core.hooksPath=/dev/null','-C',str(repo),*args],env=env,check=True,capture_output=True,timeout=30).stdout
        def capture():
            data=io.BytesIO()
            with tarfile.open(fileobj=data,mode='w') as archive: archive.add(repo,arcname='.')
            files,inventory=select_repository(data.getvalue())
            for name in source:
                if name.startswith('home/'): files[name]=(workspace/name).read_bytes()
            files['host/git-head.txt']=git('rev-parse','HEAD'); files['host/git-stage.txt']=git('ls-files','--stage')
            files['host/git-status.txt']=git('status','--porcelain=v1','--untracked-files=all')
            files['host/repository-inventory.json']=(json.dumps(inventory,sort_keys=True,separators=(',',':'))+'\n').encode()
            return files
        before=capture(); save(target/'restored-pre',before)
        started={'wall_ns':time.time_ns(),'monotonic_ns':time.monotonic_ns()}
        fd=os.open(repo/'STATE.txt',os.O_WRONLY|os.O_TRUNC|os.O_NOFOLLOW)
        with os.fdopen(fd,'wb') as f: f.write(b'fault-injected\n')
        ended={'wall_ns':time.time_ns(),'monotonic_ns':time.monotonic_ns()}
        after=capture(); save(target/'post',after)
        change=verify_injection({k:v for k,v in before.items() if k!='host/repository-inventory.json'},
                                {k:v for k,v in after.items() if k!='host/repository-inventory.json'})
        (target/'injection.json').write_text(json.dumps({'change':change,'started':started,'ended':ended}))
        result=verify(packet,candidate,target)
        return {'mode':'local-noauth-synthetic-injection','restore':proof,'boundary':result,'workspace_removed_after_test':True}

if __name__ == '__main__':
    if len(sys.argv)!=4: raise SystemExit('usage: local_probe.py PACKET CANDIDATE SCRATCH')
    print(json.dumps(main(*sys.argv[1:]),sort_keys=True,indent=2))
