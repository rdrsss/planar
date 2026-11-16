"""Install the reviewed portable fixture in an empty isolated workspace offline."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

from materialize_v2 import derive_packet

FIXTURE = Path(__file__).resolve().parent / 'fixture-v2'
ORIGINAL = Path(__file__).resolve().parent / 'fixture'
BASE = '08bacc341a31f32c14a1695b5b2de5c44b3a3e56'


def install(workspace, fixture=FIXTURE):
    workspace, fixture = Path(workspace), Path(fixture)
    if workspace.is_symlink() or not workspace.is_dir() or any(workspace.iterdir()):
        raise ValueError('workspace must be an empty regular directory')
    inventory = json.loads((fixture / 'inventory.json').read_text())
    if inventory['schema'] != 2 or inventory.get('derivation', {}).get('version') != 'v2-clean-review-boundary' or inventory['base'] != BASE or len(inventory['files']) != 32:
        raise ValueError('invalid fixture inventory')
    for name, entry in inventory['files'].items():
        p = Path(name)
        if p.is_absolute() or '..' in p.parts or not name.startswith(('home/', 'repository/')):
            raise ValueError('unsafe fixture path')
        path = fixture / p
        if any(node.is_symlink() for node in (path, *list(path.parents)[:len(p.parts)-1])) or not path.is_file():
            raise ValueError('unsafe fixture file')
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry['portable_sha256']:
            raise ValueError('fixture hash mismatch')
    old = json.loads((ORIGINAL/'inventory.json').read_text())
    if old.get('schema') != 1 or set(old['files']) != set(inventory['files']):
        raise ValueError('historical portable inventory missing or inconsistent')
    packet = 'home/projects/revision-fault/tasks/T-001/reviewer-packet-1.md'
    worker = 'home/projects/revision-fault/tasks/T-001/worker-packet-1.md'
    for name, entry in inventory['files'].items():
        old_bytes = (ORIGINAL/name).read_bytes()
        if hashlib.sha256(old_bytes).hexdigest() != old['files'][name]['portable_sha256']:
            raise ValueError('historical portable bytes changed')
        if name != packet and old_bytes != (fixture/name).read_bytes():
            raise ValueError('unexpected v2 transformation')
    if derive_packet((ORIGINAL/packet).read_bytes(), (ORIGINAL/worker).read_bytes()) != (fixture/packet).read_bytes():
        raise ValueError('v2 reviewer packet derivation mismatch')
    env = {**os.environ, 'GIT_CONFIG_NOSYSTEM':'1', 'GIT_CONFIG_GLOBAL':os.devnull,
           'GIT_TERMINAL_PROMPT':'0'}
    def git(*args):
        return subprocess.run(['git', *args], check=True, capture_output=True, text=True, env=env).stdout
    bundle = (fixture/'base.bundle').resolve(strict=True)
    if hashlib.sha256(bundle.read_bytes()).hexdigest() != 'bda65aa3aefc9bfc61b878e32129acb716a7bdc9b13d10c4b3ea936052887b41':
        raise ValueError('base bundle hash mismatch')
    git('clone', '--no-local', '--no-hardlinks', str(bundle), str(workspace/'repository'))
    repo = workspace/'repository'
    git('-C', str(repo), 'checkout', '--detach', BASE)
    if git('-C', str(repo), 'rev-parse', 'HEAD').strip() != BASE:
        raise ValueError('fixture base mismatch')
    for name in inventory['files']:
        dest = workspace/name
        dest.parent.mkdir(parents=True, exist_ok=True)
        if name.startswith('repository/') and dest.exists():
            if dest.read_bytes() != (fixture/name).read_bytes():
                raise ValueError('base tree differs from fixture')
        else:
            shutil.copyfile(fixture/name, dest)
    git('-C', str(repo), 'add', '-N', '--', 'RESULT.txt', 'STATE.txt')
    status = git('-C', str(repo), 'status', '--porcelain=v1', '--untracked-files=all')
    if status.splitlines() != [' A RESULT.txt', ' A STATE.txt']:
        raise ValueError('unexpected reconstructed index')
    if git('-C', str(repo), 'diff', '--name-only', BASE).splitlines() != ['RESULT.txt', 'STATE.txt']:
        raise ValueError('unexpected base-relative diff')
    diff = git('-C', str(repo), 'diff', '--no-ext-diff', '--no-textconv', BASE, '--', 'RESULT.txt', 'STATE.txt')
    packet = (workspace/'home/projects/revision-fault/tasks/T-001/reviewer-packet-1.md').read_text()
    expected = packet.split('## Change\n\n~~~~diff\n', 1)[1].split('~~~~', 1)[0]
    if diff != expected:
        raise ValueError('reconstructed diff differs from retained packet')
    return {'base':BASE, 'index':'intent-to-add reconstructed', 'files':len(inventory['files']), 'fixture_version':'v2-clean-review-boundary'}
