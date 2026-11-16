"""No-model synthetic control for the selected recovery Harbor pipeline only."""
import json
import os
import shutil
import sys
from pathlib import Path

from smoke_codex import SmokeCodex, CONFIG, HOME, ENV, ROOT


class SyntheticRecoveryControl(SmokeCodex):
    async def run(self, instruction, environment, context):
        if (not self._execution_id.startswith('planar-t044-noauth-') or
            self._case_name != 'fresh-interrupted-review' or
            os.environ.get('CODEX_AUTH_JSON_PATH') or os.environ.get('CODEX_FORCE_AUTH_JSON') or
            os.environ.get('OPENAI_API_KEY') or os.environ.get('CODEX_API_KEY')):
            raise ValueError('synthetic control requires isolated no-auth recovery trial')
        variant = os.environ.get('PLANAR_T045_CONTROL_VARIANT')
        if variant not in ('positive', 'changed-check'):
            raise ValueError('unknown synthetic control variant')
        case = ROOT/'evals/planar/cases/fresh-interrupted-review'
        sys.path.insert(0, str(case))
        from test_assess import IntegratedDraft
        control = IntegratedDraft('test_bad_verdict')
        control.setUp()
        try:
            control.install_direct_preflight()
            control.use_canonical_retry()
            control.use_preflight_session_and_arbitrary_retry()
            (control.root/'home/projects/revision-fault/tasks/T-001/custom-preflight-note.md').write_text('synthetic unlinked note; no assertion credit\n')
            if variant == 'changed-check':
                check = control.root/'home/projects/revision-fault/tasks/T-001/checks-1.md'
                check.write_bytes(check.read_bytes()+b'!')
            retained = self._provenance_dir/'synthetic-inputs'
            retained.mkdir(mode=0o700)
            shutil.copytree(control.root/'home', retained/'home')
            (retained/'repository').mkdir()
            for name in ('README.md', 'RESULT.txt', 'STATE.txt'):
                shutil.copyfile(control.root/'repository'/name, retained/'repository'/name)
            logs = retained/'agent/sessions/2026/01/01'; logs.mkdir(parents=True)
            for name, events in (('parent', control.parent), ('child', control.child)):
                (logs/f'rollout-{name}.jsonl').write_text('\n'.join(json.dumps(e) for e in events)+'\n')
            (retained/'control.json').write_text(json.dumps({'synthetic':True, 'variant':variant, 'model_invoked':False})+'\n')
            await self._upload_effective_config(environment, CONFIG, HOME+'/config.toml')
            task = retained/'home/projects/revision-fault/tasks/T-001'
            names = ('record.md', 'session-review-preflight-1-1.md',
                     'review-1-all.md', 'custom-preflight-note.md', 'session-reviewer-retry-1-1.md', 'session-reviewer-retry-1-2.md', 'session-reviewer-retry-1-3.md')
            if variant == 'changed-check': names += ('checks-1.md',)
            for name in names:
                await environment.upload_file(task/name, '/workspace/home/projects/revision-fault/tasks/T-001/'+name)
            for name in ('run-status.md', 'cleanup-resume-20261116-1.md'):
                await environment.upload_file(retained/'home/projects/revision-fault/reviews'/name, '/workspace/home/projects/revision-fault/reviews/'+name)
            await self.exec_as_root(environment, command='mkdir -p /logs/agent/sessions/2026/01/01', timeout_sec=30)
            for path in logs.iterdir():
                await environment.upload_file(path, '/logs/agent/sessions/2026/01/01/'+path.name)
            self._record('planar-synthetic-control.json', {'synthetic':True, 'variant':variant, 'model_invoked':False})
        finally:
            control.doCleanups()
            await self.exec_as_root(environment, command='rm -rf /installed-agent/codex-home /tmp/planar-recovery-fixture && test ! -e /installed-agent/codex-home && test ! -e /tmp/planar-recovery-fixture', timeout_sec=30)


class RecordedLiveRecoveryControl(SmokeCodex):
    """Replay one fixed retained export through Harbor; never run a model."""
    async def run(self, instruction, environment, context):
        if (not self._execution_id.startswith('planar-t044-noauth-') or
            self._case_name != 'fresh-interrupted-review' or
            os.environ.get('PLANAR_T045_CONTROL_VARIANT') != 'recorded-live' or
            os.environ.get('CODEX_AUTH_JSON_PATH') or os.environ.get('CODEX_FORCE_AUTH_JSON') or
            os.environ.get('OPENAI_API_KEY') or os.environ.get('CODEX_API_KEY')):
            raise ValueError('recorded control requires isolated no-auth recovery trial')
        source_id = 'planar-t044-ecfefbd126dd46848b6cac4e8536fcd1'
        source = ROOT.parent/'reviews/harbor-t044-jobs'/source_id/source_id/'fresh-interrupted-review__2ekwojJ/artifacts'
        retained = self._provenance_dir/'recorded-live-inputs'
        retained.mkdir(mode=0o700)
        try:
            for part in ('workspace/home', 'workspace/repository', 'logs/agent'):
                root = source/part
                if root.is_symlink() or not root.is_dir(): raise ValueError('recorded input unavailable')
                for path in root.rglob('*'):
                    if path.is_symlink(): raise ValueError('recorded input symlink')
                    if path.is_file() and path.stat().st_size > 4*1024*1024: raise ValueError('recorded input oversized')
                shutil.copytree(root, retained/part)
            (retained/'control.json').write_text(json.dumps({'synthetic':False, 'recorded_live':True, 'source_execution':source_id, 'model_invoked':False})+'\n')
            await self._upload_effective_config(environment, CONFIG, HOME+'/config.toml')
            for path in sorted((retained/'workspace/home').rglob('*')):
                if path.is_file():
                    rel = path.relative_to(retained/'workspace/home').as_posix()
                    await environment.upload_file(path, '/workspace/home/'+rel)
            for name in ('README.md','RESULT.txt','STATE.txt'):
                await environment.upload_file(retained/'workspace/repository'/name, '/workspace/repository/'+name)
            await self.exec_as_root(environment, command='mkdir -p /logs/agent/sessions/2026/11/16', timeout_sec=30)
            for path in sorted((retained/'logs/agent').rglob('*')):
                if path.is_file():
                    rel = path.relative_to(retained/'logs/agent').as_posix()
                    await environment.upload_file(path, '/logs/agent/'+rel)
            self._record('planar-recorded-control.json', {'synthetic':False, 'recorded_live':True, 'source_execution':source_id, 'model_invoked':False})
        finally:
            await self.exec_as_root(environment, command='rm -rf /installed-agent/codex-home /tmp/planar-recovery-fixture && test ! -e /installed-agent/codex-home && test ! -e /tmp/planar-recovery-fixture', timeout_sec=30)
