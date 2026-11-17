"""One no-model Docker capture/injection control; output root must be new."""
import asyncio
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

CASE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(CASE))
from capture import census, collect, inject, save

IMAGE = 'sha256:06e95472c9fb2a7ab49c50b0f2c87f5f1006aa33397ef17715a75164458b7ed2'

def docker(*args):
    return subprocess.run(['docker', *args], check=True, capture_output=True, text=True, timeout=30).stdout

class Environment:
    def __init__(self, cid): self.cid = cid
    async def _archive(self, path):
        result = subprocess.run(['docker','cp', self.cid+':'+path, '-'], check=True, capture_output=True, timeout=30)
        if len(result.stdout) > 8*1024*1024: raise ValueError('archive bound exceeded')
        return result.stdout

async def main():
    if len(sys.argv) != 2: raise ValueError('new evidence root required')
    evidence = Path(sys.argv[1])
    if evidence.exists() or evidence.is_symlink(): raise ValueError('evidence root exists')
    evidence.mkdir(mode=0o700)
    for name in ('OPENAI_API_KEY','CODEX_API_KEY','CODEX_AUTH_JSON_PATH','CODEX_FORCE_AUTH_JSON'):
        os.environ.pop(name, None)
    spec = importlib.util.spec_from_file_location('clean_fixture', CASE/'install_fixture.py')
    fixture = importlib.util.module_from_spec(spec); spec.loader.exec_module(fixture)
    cid = None
    try:
        with tempfile.TemporaryDirectory(prefix='planar-capture-fixture-') as tmp:
            fixture.install(tmp)
            cid = docker('run','-d','--network','none','--entrypoint','sleep',IMAGE,'300').strip()
            if len(cid) != 64: raise RuntimeError('container identity missing')
            docker('exec','--user','root',cid,'mkdir','-p','/workspace')
            docker('cp',tmp+'/home',cid+':/workspace/home')
            docker('cp',tmp+'/repository',cid+':/workspace/repository')
            baseline = census(cid)
            env = Environment(cid)
            initial = await collect(env,cid,baseline); save(evidence/'initial',initial)
            docker('exec','--user','root',cid,'python3','-c',"from pathlib import Path; Path('/workspace/repository/RESULT.txt').write_bytes(b'2\\n'); Path('/workspace/repository/STATE.txt').write_bytes(b'correct\\n')")
            before = await collect(env,cid,baseline); save(evidence/'pre',before)
            after, receipt = await inject(env,cid,baseline,before); save(evidence/'post',after)
            docker('exec','-d','--user','root',cid,'python3','-c','import time; time.sleep(30)','planar-census-probe')
            try:
                await collect(env,cid,baseline)
            except ValueError as error:
                if 'quiescent' not in str(error): raise
            else:
                raise RuntimeError('live descendant was not rejected')
            (evidence/'receipt.json').write_text(json.dumps({'synthetic':True,'container_id':cid,'baseline':baseline,'injection':receipt,'live_process_rejected':True},indent=2)+'\n')
    finally:
        if cid:
            docker('rm','-f',cid)
            remaining = docker('ps','-aq','--no-trunc','--filter','id='+cid).splitlines()
            (evidence/'cleanup.json').write_text(json.dumps({'container_id':cid,'remaining':remaining})+'\n')
            if remaining: raise RuntimeError('container cleanup unconfirmed')
    print(str(evidence))

if __name__ == '__main__': asyncio.run(main())
