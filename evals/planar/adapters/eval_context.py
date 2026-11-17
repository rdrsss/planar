"""Explicit Planar evaluation project and evidence selection (TS-19.10)."""
from dataclasses import dataclass, asdict
import os
from pathlib import Path
import re
import json
import hashlib
import subprocess
import yaml


class UniqueLoader(yaml.SafeLoader):
    pass


def _mapping(loader, node, deep=False):
    result = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if not isinstance(key, str) or key in result:
            raise ValueError('duplicate or invalid metadata key')
        result[key] = loader.construct_object(value_node, deep=deep)
    return result


UniqueLoader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _mapping)

@dataclass(frozen=True)
class Context:
    schema: str
    settings_home: str
    settings_home_source: str
    settings_file_sha256: str
    project_dir: str
    project_reviews_dir: str
    output_root: str
    jobs_root: str
    consumed_root: str
    account_scope: str
    layout_kind: str
    selection_source: str

    def identity(self):
        return asdict(self)

    def digest(self):
        return hashlib.sha256(json.dumps(self.identity(), sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def _path(value, label, *, exists=False):
    if not isinstance(value, str) or not value or not (value.startswith('/') or value.startswith('~/')):
        raise ValueError(f'{label} must be an absolute path or ~/ path')
    path = Path(value).expanduser()
    if path != path.resolve(strict=exists):
        raise ValueError(f'{label} must not contain symlink aliases or ..')
    return path


def _yaml(path, raw=None):
    if path.is_symlink() or not path.is_file():
        raise ValueError(f'missing or unsafe metadata: {path}')
    try:
        result = yaml.load(path.read_bytes() if raw is None else raw, Loader=UniqueLoader)
    except (OSError, yaml.YAMLError) as exc:
        raise ValueError(f'invalid metadata: {path}') from exc
    if not isinstance(result, dict):
        raise ValueError(f'invalid metadata: {path}')
    return result


def _common(path):
    result = subprocess.run(['git', '-C', str(path), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True)
    if result.returncode:
        raise ValueError('project repository identity unavailable')
    return Path(result.stdout.strip()).resolve(strict=True)


def resolve(repo, *, historical=False):
    selected = os.environ.get('PLANAR_EVALS_PROJECT_DIR')
    if not selected:
        raise ValueError('set PLANAR_EVALS_PROJECT_DIR to the selected project directory')
    project = _path(selected, 'project directory', exists=True)
    if not project.is_dir(): raise ValueError('project directory unavailable')
    metadata = _yaml(project / 'project.yaml')
    name = metadata.get('name')
    if not isinstance(name, str) or not re.fullmatch(r'[a-z0-9][a-z0-9-]*', name): raise ValueError('invalid project name')
    home_value = os.environ.get('PLANAR_HOME')
    home = _path(home_value if home_value is not None else str(Path.home()/'.planar'), 'settings home', exists=True)
    if not home.is_dir(): raise ValueError('settings home unavailable')
    home_source = 'explicit' if home_value is not None else 'default'
    settings_path = home/'settings.yaml'
    if settings_path.is_symlink(): raise ValueError('unsafe settings file')
    if settings_path.exists():
        if not settings_path.is_file(): raise ValueError('settings must be a regular file')
        settings_raw = settings_path.read_bytes()
        settings = _yaml(settings_path, settings_raw)
        settings_hash = hashlib.sha256(settings_raw).hexdigest()
    else:
        settings, settings_hash = {}, 'absent'
    projects = settings.get('projects', {})
    if not isinstance(projects, dict): raise ValueError('invalid projects settings')
    entry = projects.get(name, {})
    if not isinstance(entry, dict): raise ValueError('invalid project settings')
    base = settings.get('projects_dir', str(home/'projects'))
    configured = _path(entry.get('path', str(_path(base, 'projects_dir')/name)), 'configured project directory', exists=True)
    if configured != project: raise ValueError('selected project does not match settings')
    repository = metadata.get('repository')
    if not isinstance(repository, dict): raise ValueError('invalid project repository')
    repository_path = _path(repository.get('path'), 'project repository', exists=True)
    checkout = Path(repo).resolve(strict=True)
    if _common(repository_path) != _common(checkout): raise ValueError('project repository mismatch')
    override = os.environ.get('PLANAR_EVALS_OUTPUT_ROOT')
    kind = os.environ.get('PLANAR_EVALS_LAYOUT_KIND')
    if historical:
        if not override or kind not in ('legacy-reviews', 'override-jobs'):
            raise ValueError('historical recovery requires exact output root and layout kind')
        output = _path(override, 'historical output root', exists=True)
        if kind == 'legacy-reviews' and output != project/'reviews': raise ValueError('legacy reviews root mismatch')
        source = 'historical'
    else:
        if kind and kind != 'new': raise ValueError('historical layout cannot be used for new work')
        output = _path(override, 'output root') if override else project/'scratch'/'scratch-evals'
        output = _path(str(output), 'output root')
        kind, source = 'new', 'explicit override' if override else 'project default'
        # No output may contain the checkout or lie within a worktree. A project-
        # local output is allowed only under the real scratch directory.
        listing = subprocess.run(['git', '-C', str(checkout), 'worktree', 'list', '--porcelain'], capture_output=True, text=True)
        if listing.returncode: raise ValueError('worktree inventory unavailable')
        trees = [Path(line[9:]).resolve() for line in listing.stdout.splitlines() if line.startswith('worktree ')]
        if any(output == tree or output in tree.parents or tree in output.parents for tree in trees):
            raise ValueError('output overlaps a registered worktree')
        if output.exists() and not output.is_dir(): raise ValueError('output root is not a directory')
        default_root = project/'scratch'/'scratch-evals'
        for owned in (default_root/'jobs', default_root/'harbor-t044-consumed', default_root/'planar-account-admission-v1'):
            if output == owned or owned in output.parents:
                raise ValueError('output overlaps an existing owned job or account root')
        if output == project or output in project.parents:
            raise ValueError('output overlaps project')
        if project in output.parents and not output.is_relative_to(project/'scratch'):
            raise ValueError('project-local output must be under scratch')
        if project in output.parents and (len(output.relative_to(project/'scratch').parts) < 1 or not output.relative_to(project/'scratch').parts[0].startswith('scratch-')):
            raise ValueError('project-local output must use scratch-*')
    jobs = output/('harbor-t044-jobs' if kind == 'legacy-reviews' else 'jobs')
    consumed = output/'harbor-t044-consumed'
    for path in (jobs, consumed):
        if path.is_symlink() or path != path.resolve(): raise ValueError('output destination is a symlink alias')
    context = Context('planar-eval-context-v1', str(home), home_source, settings_hash, str(project), str(project/'reviews'), str(output), str(jobs), str(consumed), str(output/'planar-account-admission-v1'), kind, source)
    forwarded = os.environ.get('PLANAR_EVALS_CONTEXT')
    if forwarded:
        try: expected = json.loads(forwarded)
        except ValueError: raise ValueError('invalid forwarded context') from None
        if expected != context.identity(): raise ValueError('forwarded context mismatch')
    return context
