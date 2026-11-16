"""Fail-closed host-side provenance cross-check; never upgrades a verifier result."""
from __future__ import annotations

import json
import hashlib
from pathlib import Path
import sys


def check(directory: Path):
    directory = directory.resolve(strict=True)
    def load(name):
        path = directory / name
        if path.is_symlink() or not path.is_file():
            raise ValueError('required host provenance is absent or not a regular file')
        value = json.loads(path.read_text())
        if not isinstance(value, dict):
            raise ValueError('malformed host provenance')
        return value
    start = load('launcher.json')
    execution_id = start.get('execution_id')
    if not isinstance(execution_id, str) or execution_id != directory.name:
        raise ValueError('execution identity mismatch')
    config_path = directory / 'job-config.json'
    if config_path.is_symlink() or hashlib.sha256(config_path.read_bytes()).hexdigest() != start.get('job_config_sha256'):
        raise ValueError('job configuration identity mismatch')
    records = [load(name) for name in ('planar-image.json', 'planar-cli.json', 'planar-install-provenance.json', 'planar-launch.json', 'planar-terminal.json', 'planar-cleanup.json', 'launcher-terminal.json')]
    if any(record.get('execution_id') != execution_id for record in records):
        raise ValueError('execution identity mismatch')
    image, cli, install, launch, terminal, cleanup, launcher_terminal = records
    if cli.get('version') != 'codex-cli 0.169.0':
        raise ValueError('CLI version mismatch')
    if not isinstance(image.get('id'), str) or not image['id'].startswith('sha256:'):
        raise ValueError('image identity missing')
    if install.get('candidate_sha256') != start.get('candidate_sha256') or install.get('plugin_id') != 'planar@planar-smoke-market':
        raise ValueError('installed candidate identity mismatch')
    if install.get('installed_path') != '/installed-agent/codex-home/plugins/cache/planar-smoke-market/planar/0.1.0' or install.get('list_contains_plugin') is not True:
        raise ValueError('installed plugin location or list mismatch')
    if launch.get('cli_version') != '0.169.0' or launch.get('model') != 'gpt-6.1-terra' or 'codex exec ' not in launch.get('command', ''):
        raise ValueError('native launch mismatch')
    if terminal.get('status') != 'shell_completed' or terminal.get('return_code') != 0 or cleanup.get('status') != 'passed' or launcher_terminal.get('status') != 'completed':
        raise ValueError('execution or cleanup incomplete')
    return {'status': 'inconclusive', 'reason': 'host provenance cross-check passed; native terminal and independent dispatch verdict still required'}


def main():
    try:
        print(json.dumps(check(Path(sys.argv[1]))))
    except (IndexError, OSError, ValueError, json.JSONDecodeError):
        print(json.dumps({'status': 'infrastructure_error', 'reason': 'host provenance missing, malformed or inconsistent'}))
    return 2


if __name__ == '__main__':
    sys.exit(main())
