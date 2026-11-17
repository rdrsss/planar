"""Host-controlled capture for an identified quiescent container."""
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import tarfile
import subprocess
import time

import importlib.util

CASE = Path(__file__).resolve().parent

def _sibling(name):
    path=CASE/(name+'.py')
    if path.is_symlink() or not path.is_file(): raise ValueError('unsafe case helper')
    spec=importlib.util.spec_from_file_location('_planar_work_capture_'+name,path)
    if spec is None or spec.loader is None: raise ValueError('case helper unavailable')
    module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module

_boundary=_sibling('boundary'); _exports=_sibling('safe_export'); _snapshot=_sibling('snapshot')
manifest=_boundary.manifest
MAX_ARCHIVE=_exports.MAX_ARCHIVE
select_recovery_home=_exports.select_recovery_home
write_tree=_exports.write_tree
select_repository=_snapshot.select_repository
PROJECT = 'projects/revision-fault/'


def _docker(*args):
    return subprocess.run(['docker', *args], check=True, capture_output=True, text=True, timeout=30).stdout


def census(container_id):
    if not re.fullmatch(r'[0-9a-f]{64}', container_id): raise ValueError('invalid container identity')
    actual = _docker('inspect', '--format', '{{.Id}}', container_id).strip()
    if actual != container_id: raise ValueError('container identity changed')
    lines = _docker('top', container_id, '-eo', 'pid,ppid,args').splitlines()
    if len(lines) < 2: raise ValueError('process census unavailable')
    return sorted(' '.join(line.split()) for line in lines[1:])


def _git(container_id, *args):
    return _docker('exec', '--user', 'root', '--env', 'GIT_CONFIG_NOSYSTEM=1', '--env', 'GIT_CONFIG_GLOBAL=/dev/null',
                   container_id, 'git', '-c', 'safe.directory=/workspace/repository', '-c', 'core.fsmonitor=false',
                   '-c', 'core.hooksPath=/dev/null', '-C', '/workspace/repository', *args)



def retain_rejected_home(data, destination):
    """Save only the bounded, unextracted home archive in private host evidence."""
    destination = Path(destination)
    info = destination.lstat()
    if (destination.is_symlink() or not stat.S_ISDIR(info.st_mode) or
        info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) & 0o077):
        raise ValueError('unsafe rejected-archive destination')
    if not isinstance(data, bytes) or len(data) > MAX_ARCHIVE:
        raise ValueError('invalid rejected-archive size')
    path = destination/'rejected-home.tar'
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'wb') as output:
        output.write(data); output.flush(); os.fsync(output.fileno())
    receipt = {'reason':'bounded_home_selector_rejected', 'source':'/workspace/home/.',
               'size':len(data), 'sha256':hashlib.sha256(data).hexdigest()}
    fd = os.open(destination/'rejected-home.json', os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as output:
        json.dump(receipt, output, sort_keys=True); output.write('\n'); output.flush(); os.fsync(output.fileno())
    return receipt


async def collect(environment, container_id, baseline, *, failure_evidence=None):
    """Collect full bounded evidence only while exact process census matches."""
    if not baseline or census(container_id) != baseline: raise ValueError('container not quiescent')
    head = _git(container_id, 'rev-parse', 'HEAD')
    stage = _git(container_id, 'ls-files', '--stage')
    status = _git(container_id, 'status', '--porcelain=v1', '--untracked-files=all')
    diff = _git(container_id, 'diff', '--no-ext-diff', 'HEAD', '--', 'RESULT.txt', 'STATE.txt')
    if head.strip() != '08bacc341a31f32c14a1695b5b2de5c44b3a3e56': raise ValueError('base changed')
    projection = json.loads((CASE/'projection.json').read_text())['files']
    retained = {name[5:] for name in projection if name.startswith('home/')}
    archive = await environment._archive('/workspace/home/.')
    try:
        home = select_recovery_home(archive, retained)
    except (ValueError, tarfile.TarError):
        if failure_evidence is not None: retain_rejected_home(archive, failure_evidence)
        raise
    for name in retained:
        if name not in home: raise ValueError('initial home input missing')
        # tasks.md is allowed to change at the final step; the verifier checks it.
        if name != PROJECT+'tasks.md' and hashlib.sha256(home[name]).hexdigest() != projection['home/'+name]:
            raise ValueError('immutable home input changed')
    repository, repo_meta = select_repository(await environment._archive('/workspace/repository/.'))
    if hashlib.sha256(repository['repository/README.md']).hexdigest() != projection['repository/README.md']:
        raise ValueError('base README changed')
    if repository['repository/.git/HEAD'] != b'08bacc341a31f32c14a1695b5b2de5c44b3a3e56\n':
        raise ValueError('repository is not detached at the base')
    if census(container_id) != baseline: raise ValueError('process census changed during capture')
    files = {'home/'+name: content for name,content in home.items()}
    files.update(repository)
    files['host/git-head.txt'] = head.encode()
    files['host/git-stage.txt'] = stage.encode()
    files['host/git-status.txt'] = status.encode()
    files['host/task-diff.txt'] = diff.encode()
    files['host/repository-inventory.json'] = (json.dumps(repo_meta, sort_keys=True, separators=(',', ':'))+'\n').encode()
    return files


def save(destination, files):
    """Write a one-time immutable host snapshot and host-computed manifest."""
    destination = Path(destination)
    if destination.exists() or destination.is_symlink(): raise ValueError('snapshot destination exists')
    inventory = manifest(files)
    write_tree(destination, files)
    descriptor = destination/'snapshot-manifest.json'
    fd = os.open(descriptor, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as output:
        json.dump(inventory, output, sort_keys=True, indent=2)
        output.write('\n')
    receipt = {'stage':destination.name, 'wall_ns':time.time_ns(), 'monotonic_ns':time.monotonic_ns(),
               'manifest_sha256':hashlib.sha256(descriptor.read_bytes()).hexdigest()}
    receipt_path = destination.parent/(destination.name+'-receipt.json')
    fd = os.open(receipt_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as output:
        json.dump(receipt, output, sort_keys=True)
        output.write('\n')


def load(destination):
    """Read a saved snapshot only when every byte matches its host manifest."""
    destination = Path(destination)
    if destination.is_symlink(): raise ValueError('unsafe snapshot directory')
    if not destination.is_dir(): raise FileNotFoundError('snapshot directory missing')
    descriptor = destination/'snapshot-manifest.json'
    if descriptor.is_symlink() or not descriptor.is_file(): raise ValueError('snapshot manifest missing')
    inventory = json.loads(descriptor.read_text())
    if not isinstance(inventory, dict) or not inventory: raise ValueError('invalid snapshot manifest')
    actual = set()
    for path in destination.rglob('*'):
        if path.is_symlink(): raise ValueError('symlink in snapshot')
        if path.is_file() and path != descriptor: actual.add(path.relative_to(destination).as_posix())
        elif not path.is_file() and not path.is_dir(): raise ValueError('special snapshot entry')
    if actual != set(inventory): raise ValueError('snapshot inventory mismatch')
    files = {}
    for name, item in inventory.items():
        if not isinstance(name, str) or name.startswith('/') or any(part in ('', '.', '..') for part in name.split('/')):
            raise ValueError('unsafe snapshot name')
        path = destination/name
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        try:
            with os.fdopen(fd, 'rb') as stream: data = stream.read(4*1024*1024+1)
        except BaseException:
            raise
        if len(data) > 4*1024*1024 or item != {'size':len(data), 'sha256':hashlib.sha256(data).hexdigest()}:
            raise ValueError('snapshot content mismatch')
        files[name] = data
    return files
