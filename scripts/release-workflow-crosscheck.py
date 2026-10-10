#!/usr/bin/env python3
"""Manual cross-check of .github/workflows/release.yml against independent tools.

Not part of ctest: it needs PyYAML and actionlint, which are not test dependencies.
It checks that the in-tree YAML reader in release-workflow.test.py agrees with PyYAML
on every repository workflow, and that actionlint accepts release.yml. A missing
tool is a failure, never a skip.
"""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def main():
    try:
        import yaml
    except ImportError:
        print('release-workflow-crosscheck: PyYAML is not installed (pip install pyyaml)', file=sys.stderr)
        return 2
    tool = shutil.which('actionlint')
    if tool is None:
        print('release-workflow-crosscheck: actionlint is not installed', file=sys.stderr)
        return 2
    spec = importlib.util.spec_from_file_location('release_workflow_test', ROOT / 'scripts/release-workflow.test.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    def norm(v):
        if isinstance(v, dict):
            return {k: norm(x) for k, x in v.items()}
        if isinstance(v, list):
            return [norm(x) for x in v]
        return '' if v is None else str(v).lower() if isinstance(v, bool) else str(v)

    failed = False
    for path in sorted((ROOT / '.github/workflows').glob('*.yml')):
        expected = yaml.load(path.read_text(), Loader=yaml.BaseLoader)
        got = json.loads(json.dumps(module.parse_yaml(path.read_text())))
        same = norm(got) == norm(expected)
        print(f'{path.name}: reader {"agrees with" if same else "DISAGREES with"} PyYAML')
        failed |= not same
    result = subprocess.run([tool, str(ROOT / '.github/workflows/release.yml')], capture_output=True, text=True)
    print(f'actionlint release.yml: exit {result.returncode}')
    if result.returncode:
        print(result.stdout + result.stderr, file=sys.stderr)
        failed = True
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
