"""Install the fixed initial projection, with no historical task artifacts."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

CASE = Path(__file__).resolve().parent
SOURCE = CASE / 'fixture'
BASE = '08bacc341a31f32c14a1695b5b2de5c44b3a3e56'
BUNDLE = 'bda65aa3aefc9bfc61b878e32129acb716a7bdc9b13d10c4b3ea936052887b41'
PATHS = frozenset(('home/settings.yaml', 'home/profiles/revision-fault.yaml',
    'home/projects/revision-fault/context.md', 'home/projects/revision-fault/ids.md',
    'home/projects/revision-fault/layout.md', 'home/projects/revision-fault/prd.md',
    'home/projects/revision-fault/project.yaml', 'home/projects/revision-fault/reviews/spec-check-1.md',
    'home/projects/revision-fault/tasks.md', 'home/projects/revision-fault/tech-spec.md',
    'home/projects/revision-fault/testing-spec.md', 'repository/README.md'))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def install(workspace, source=SOURCE, manifest=CASE / 'projection.json'):
    workspace, source, manifest = Path(workspace), Path(source), Path(manifest)
    if workspace.is_symlink() or not workspace.is_dir() or any(workspace.iterdir()):
        raise ValueError('workspace must be an empty regular directory')
    data = json.loads(manifest.read_text())
    original = json.loads((source / 'inventory.json').read_text())
    if data.get('base') != BASE or data.get('bundle_sha256') != BUNDLE or set(data.get('files', {})) != PATHS:
        raise ValueError('projection identity mismatch')
    for name, expected in data['files'].items():
        path = source / name
        if any(p.is_symlink() for p in (path, *list(path.parents)[:len(Path(name).parts)-1])) or not path.is_file():
            raise ValueError('unsafe projection input')
        if original['files'][name]['portable_sha256'] != expected or digest(path) != expected:
            raise ValueError('projection input hash mismatch')
    bundle = source / 'base.bundle'
    if bundle.is_symlink() or not bundle.is_file() or digest(bundle) != BUNDLE:
        raise ValueError('bundle identity mismatch')
    config = (source / 'home/projects/revision-fault/project.yaml').read_text()
    if '  path: /workspace/repository\n' not in config or '  status: approved\n' not in config:
        raise ValueError('project config mismatch')
    for name in ('prd.md', 'tech-spec.md', 'layout.md', 'testing-spec.md', 'tasks.md'):
        if f'    {name}: {data["files"]["home/projects/revision-fault/" + name]}\n' not in config:
            raise ValueError('approved digest mismatch')
    env = {**os.environ, 'GIT_CONFIG_NOSYSTEM': '1', 'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_TERMINAL_PROMPT': '0'}
    def git(*args):
        return subprocess.run(['git', *args], check=True, capture_output=True, text=True, env=env).stdout
    repo = workspace / 'repository'
    git('clone', '--no-local', '--no-hardlinks', str(bundle.resolve()), str(repo))
    git('-C', str(repo), 'checkout', '--detach', BASE)
    if git('-C', str(repo), 'rev-parse', 'HEAD').strip() != BASE or git('-C', str(repo), 'status', '--porcelain=v1', '--untracked-files=all'):
        raise ValueError('base checkout is not clean')
    if (repo / 'RESULT.txt').exists() or (repo / 'STATE.txt').exists() or digest(repo / 'README.md') != data['files']['repository/README.md']:
        raise ValueError('base content mismatch')
    for name in sorted(PATHS):
        if name.startswith('home/'):
            dest = workspace / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / name, dest)
    if (workspace / 'home/projects/revision-fault/tasks/T-001').exists():
        raise ValueError('historical task subtree exists')
    return {'base': BASE, 'bundle_sha256': BUNDLE, 'projection_files': len(PATHS), 'index': 'clean base', 'outputs': 'absent'}
