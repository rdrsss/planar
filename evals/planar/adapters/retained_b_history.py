"""Reproduce the reviewed retained-B implementation inventory without executing it."""
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile

COMMIT = 'bd3fecfe1c4786125973fc800150173f52be1f53'
TREE = '543e200ca8fc47293f175d275e6847f2d92234a9'
INVENTORY = '85534f2bdbc2d7027ce26dd6939b55ed4af5212c1cfdad2886fe0bb921b23a33'
PATHS = ('evals/pyproject.toml','evals/uv.lock','evals/README.md','evals/planar/README.md',
         'evals/planar/adapters','evals/planar/cases/retained-a-fresh-b','evals/planar/experiments/codex_run.py')


def historical_inventory(repository):
    repository=Path(repository).resolve(strict=True)
    tree=subprocess.run(['git','rev-parse',COMMIT+'^{tree}'],cwd=repository,capture_output=True,text=True,check=True,timeout=15).stdout.strip()
    if tree != TREE: raise ValueError('historical commit tree mismatch')
    archive=subprocess.run(['git','archive','--format=tar',COMMIT,*PATHS],cwd=repository,capture_output=True,check=True,timeout=30).stdout
    with tempfile.TemporaryDirectory(prefix='planar-retained-history-') as temporary:
        root=Path(temporary)
        with tarfile.open(fileobj=io.BytesIO(archive),mode='r:') as tar:
            seen=set()
            for member in tar.getmembers():
                path=Path(member.name)
                if path.is_absolute() or '..' in path.parts or member.name in seen or not (member.isdir() or member.isfile()):
                    raise ValueError('unsafe historical archive entry')
                seen.add(member.name)
                target=root/path
                if member.isdir(): target.mkdir(parents=True,exist_ok=True)
                else:
                    target.parent.mkdir(parents=True,exist_ok=True)
                    source=tar.extractfile(member)
                    if source is None: raise ValueError('historical archive file missing')
                    target.write_bytes(source.read())
                    target.chmod(0o400)
        files=[root/x for x in PATHS[:4]]+[root/PATHS[-1]]
        for directory in (root/PATHS[4],root/PATHS[5]): files.extend(p for p in directory.rglob('*') if p.is_file() and '__pycache__' not in p.parts)
        records=[(str(p.relative_to(root)),hashlib.sha256(p.read_bytes()).hexdigest()) for p in sorted(files)]
        actual=hashlib.sha256(json.dumps(records,separators=(',',':')).encode()).hexdigest()
        if actual!=INVENTORY: raise ValueError('historical implementation inventory mismatch')
        return {'commit':COMMIT,'tree':TREE,'implementation_sha256':actual}
