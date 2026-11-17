"""Version-specific Harbor 0.26.0 Codex hook. Not approved for live use yet."""
from __future__ import annotations

import asyncio
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from harbor.agents.installed.codex import Codex
from candidate import inventory
from recover import jobs_root

ROOT = Path(__file__).resolve().parents[3]
CANDIDATE = ROOT / 'plugins/planar'
MANIFEST = ROOT / 'evals/planar/cases/plugin-smoke/candidate.json'
HOME = '/installed-agent/codex-home'
CACHE = HOME + '/plugins/cache/planar-smoke-market/planar/0.1.0'
ENV = {'CODEX_HOME': HOME, 'OPENAI_API_KEY': '', 'CODEX_API_KEY': ''}
RECOVERY_SESSION_SELECTION = ('The launcher selected this fresh session with model `gpt-6.1-terra` and reasoning effort `medium` in its fixed job configuration. This is the configured selection; backend model and effort are not independently observed.')


def validate_recovery_selection(instruction, model, effort):
    if model != 'gpt-6.1-terra' or effort != 'medium' or instruction.count(RECOVERY_SESSION_SELECTION) != 1:
        raise ValueError('recovery session selection does not match the fixed launcher configuration')


CONFIG = {
    'plugins': {'_default': {'enabled': False}},
    'skills': {'bundled': {'enabled': False}},
    'features': {'apps': False, 'remote_plugin': False, 'multi_agent': True},
}


class SmokeCodex(Codex):
    """Install the approved candidate after Harbor's effective config upload."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        raw = os.environ.get('PLANAR_T044_EVIDENCE_DIR')
        if not raw:
            raise ValueError('launcher-owned provenance directory is required')
        self._provenance_dir = Path(raw).resolve(strict=True)
        if not self._provenance_dir.is_relative_to(jobs_root()):
            raise ValueError('provenance directory is outside the project evidence root')
        self._execution_id = self._provenance_dir.name
        case_name = os.environ.get('PLANAR_T044_CASE', 'plugin-smoke')
        if case_name not in ('plugin-smoke', 'native-lineage-smoke', 'native-lineage-current', 'fresh-interrupted-review'):
            raise ValueError('unsupported case')
        self._case_name = case_name
        case_snapshot = os.environ.get('PLANAR_T044_CASE_SNAPSHOT')
        candidate_snapshot = os.environ.get('PLANAR_T044_CANDIDATE_SNAPSHOT')
        if bool(case_snapshot) != bool(candidate_snapshot):
            raise ValueError('candidate and case snapshots must be paired')
        self._candidate = Path(candidate_snapshot).resolve(strict=True) if candidate_snapshot else CANDIDATE
        self._case = Path(case_snapshot).resolve(strict=True) if case_snapshot else ROOT / 'evals/planar/cases' / case_name
        self._manifest = self._case / 'candidate.json'
        if case_snapshot:
            from launch import case_inventory
            if case_inventory(self._case) != os.environ.get('PLANAR_T044_CASE_SHA256'):
                raise ValueError('case snapshot digest changed')

    def _record(self, name, value):
        # Unlike Harbor's agent logs, this directory is never mounted into the
        # agent container. Bind every record to the unique launcher execution.
        value = {'execution_id': self._execution_id, **value}
        (self._provenance_dir / name).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')

    def _image_identity(self, environment):
        image_name = environment.task_env_config.docker_image if environment._use_prebuilt else environment._main_image_name
        if not isinstance(image_name, str) or not image_name:
            raise ValueError('Docker image identity unavailable')
        result = subprocess.run(['docker', 'image', 'inspect', '--format', '{{.Id}}', image_name], capture_output=True, text=True, check=True, timeout=20)
        image_id = result.stdout.strip()
        if not image_id.startswith('sha256:') or len(image_id) != 71:
            raise ValueError('unexpected Docker image identity')
        return {'name': image_name, 'id': image_id}

    async def setup(self, environment):
        # setup precedes Harbor's auth upload in run(). Run the exact same
        # marketplace/config/cache hook in this container, without any auth.
        await super().setup(environment)
        if environment.auto_mounting:
            raise ValueError('automatic host mounts are forbidden')
        cid = await environment._platform._resolve_service_container('main')
        inspect = subprocess.run(['docker', 'inspect', '--format', '{{json .}}', cid], capture_output=True, text=True, timeout=20, check=True)
        info = json.loads(inspect.stdout)
        image_id = info.get('Image')
        full_id = info.get('Id')
        project = info.get('Config', {}).get('Labels', {}).get('com.docker.compose.project')
        if not isinstance(full_id, str) or len(full_id) != 64 or not isinstance(project, str) or not isinstance(image_id, str) or not image_id.startswith('sha256:'):
            raise ValueError('actual Docker container identity unavailable')
        expected_image = os.environ.get('PLANAR_T044_EXPECTED_IMAGE_ID')
        if expected_image and image_id != expected_image:
            raise ValueError('runtime image differs from reviewed image')
        self._record('planar-container.json', {'id': full_id, 'image_id': image_id, 'compose_project': project})
        if os.environ.get('PLANAR_T044_NOAUTH_HOLD') == '1':
            if os.environ.get('CODEX_AUTH_JSON_PATH') or os.environ.get('CODEX_FORCE_AUTH_JSON'):
                raise ValueError('no-auth interruption control cannot carry auth')
            await asyncio.sleep(60)
        await self._ensure_agent_owned_dir(environment, self._REMOTE_CODEX_HOME)
        try:
            await self._upload_effective_config(environment, CONFIG, HOME + '/config.toml')
            self._record('planar-preauth.json', {'status': 'passed', 'container_id': full_id, 'image_id': image_id})
        finally:
            if getattr(self, '_case_name', 'plugin-smoke') == 'fresh-interrupted-review':
                await self.exec_as_root(environment, command='rm -rf /workspace/home /workspace/repository /tmp/planar-recovery-fixture && test ! -e /workspace/home && test ! -e /workspace/repository && test ! -e /tmp/planar-recovery-fixture', timeout_sec=30)
            await self.exec_as_root(environment, command='rm -rf /installed-agent/codex-home && test ! -e /installed-agent/codex-home', timeout_sec=30)

    async def _upload_effective_config(self, environment, config, remote_path):
        if remote_path != HOME + '/config.toml' or config != CONFIG:
            raise ValueError('unexpected effective config or Codex home')
        if self.mcp_servers or self.skills_dir or self._resume or self._load:
            raise ValueError('only a fresh run without MCP or skill injection is supported')
        expected = json.loads(self._manifest.read_text())
        if inventory(getattr(self, '_candidate', CANDIDATE)) != expected:
            raise ValueError('candidate does not match frozen inventory')
        workspace = await self.exec_as_agent(environment, command="python3 -c 'import os,stat; p=\"/workspace\"; assert stat.S_ISDIR(os.lstat(p).st_mode) and not os.listdir(p)'", env=ENV)
        if workspace.return_code != 0:
            raise ValueError('workspace must be an empty directory before execution')
        self._record('planar-workspace-preflight.json', {'status': 'passed', 'fixture': '/workspace', 'check': 'empty-directory-before-model'})
        await super()._upload_effective_config(environment, config, remote_path)
        uid, gid = await self._ensure_agent_owned_dir(environment, self._REMOTE_CODEX_HOME)
        await self.exec_as_root(environment, command=f'chown {uid}:{gid} /installed-agent/codex-home/config.toml && chmod 600 /installed-agent/codex-home/config.toml')
        self._record('planar-image.json', self._image_identity(environment))
        version = await self.exec_as_agent(environment, command='codex --version', env=ENV)
        if version.stdout.strip() != 'codex-cli 0.169.0':
            raise ValueError('container Codex version mismatch')
        self._record('planar-cli.json', {'version': version.stdout.strip()})
        await self.exec_as_agent(environment, command='test "$HOME" = /home/agent && test ! -e "$HOME/.agents/skills" && test ! -e /workspace/AGENTS.md && test ! -e /workspace/.codex', env=ENV)
        with tempfile.TemporaryDirectory(prefix='planar-market-') as temp:
            market = Path(temp)
            market.chmod(0o755)
            shutil.copytree(getattr(self, '_candidate', CANDIDATE), market / 'plugins/planar')
            metadata = market / '.agents/plugins/marketplace.json'
            metadata.parent.mkdir(parents=True)
            metadata.write_text(json.dumps({'name': 'planar-smoke-market', 'interface': {'displayName': 'Planar smoke'}, 'plugins': [{'name': 'planar', 'source': {'source': 'local', 'path': './plugins/planar'}, 'policy': {'installation': 'AVAILABLE', 'authentication': 'ON_INSTALL'}, 'category': 'Productivity'}]}))
            await environment.upload_dir(market, '/tmp/planar-smoke-market')
        await self.exec_as_agent(environment, command='codex plugin marketplace add /tmp/planar-smoke-market', env=ENV)
        install = await self.exec_as_agent(environment, command='codex plugin add planar@planar-smoke-market --json', env=ENV)
        data = json.loads(install.stdout)
        if data.get('installedPath') != CACHE or data.get('pluginId') != 'planar@planar-smoke-market':
            raise ValueError('unexpected native installation result')
        listing = await self.exec_as_agent(environment, command='codex plugin list --json', env=ENV)
        # Inventory results must be returned from the environment, not asserted by
        # the model. The checker is uploaded separately from the candidate.
        await environment.upload_file(Path(__file__).with_name('candidate.py'), '/tmp/planar-inventory.py')
        check = await self.exec_as_agent(environment, command=f'python3 /tmp/planar-inventory.py {CACHE}', env=ENV)
        if json.loads(check.stdout) != expected:
            raise ValueError('installed candidate digest mismatch')
        config_check = await self.exec_as_agent(environment, command="python3 -c 'import json,tomllib; p=tomllib.load(open(\"/installed-agent/codex-home/config.toml\",\"rb\")); print(json.dumps(p))'", env=ENV)
        final = json.loads(config_check.stdout)
        if final.get('plugins', {}).get('planar@planar-smoke-market') != {'enabled': True}:
            raise ValueError('plugin not enabled in final config')
        if set(final.get('plugins', {})) != {'_default', 'planar@planar-smoke-market'}:
            raise ValueError('unexpected plugin entries in final config')
        if final.get('plugins', {}).get('_default') != {'enabled': False} or final.get('features') != CONFIG['features'] or final.get('skills') != CONFIG['skills']:
            raise ValueError('unexpected final isolation config')
        if set(final) - {'plugins', 'features', 'skills', 'marketplaces'}:
            raise ValueError('unexpected final config keys')
        json.loads(listing.stdout)  # malformed list output is an infrastructure error
        if 'planar@planar-smoke-market' not in listing.stdout:
            raise ValueError('installed plugin absent from native list')
        # Retain only known, non-secret fields; never export the whole home/config.
        self._record('planar-install-provenance.json', {'candidate_sha256': expected['sha256'], 'installed_path': CACHE, 'plugin_id': data['pluginId'], 'list_contains_plugin': True, 'effective_config': {'plugins': final['plugins'], 'skills': final['skills'], 'features': final['features']}})
        if getattr(self, '_case_name', 'plugin-smoke') == 'fresh-interrupted-review':
            case = getattr(self, '_case', ROOT / 'evals/planar/cases/fresh-interrupted-review')
            with tempfile.TemporaryDirectory(prefix='planar-recovery-') as temp:
                staging = Path(temp)
                staging.chmod(0o755)
                shutil.copyfile(case / 'install_fixture.py', staging / 'install_fixture.py')
                shutil.copyfile(case / 'materialize_v2.py', staging / 'materialize_v2.py')
                shutil.copytree(case / 'fixture', staging / 'fixture')
                shutil.copytree(case / 'fixture-v2', staging / 'fixture-v2')
                await environment.upload_dir(staging, '/tmp/planar-recovery-fixture')
            result = await self.exec_as_agent(environment, command="python3 -c 'import sys,json; sys.path.insert(0,\"/tmp/planar-recovery-fixture\"); from install_fixture import install; print(json.dumps(install(\"/workspace\")))'", env=ENV)
            receipt = json.loads(result.stdout)
            if receipt.get('base') != '08bacc341a31f32c14a1695b5b2de5c44b3a3e56' or receipt.get('files') != 32 or receipt.get('fixture_version') != 'v2-clean-review-boundary':
                raise ValueError('recovery fixture installation failed')
            self._record('planar-fixture-provenance.json', receipt)

    async def exec_as_agent(self, environment, command, env=None, cwd=None, timeout_sec=None):
        if getattr(self, '_case_name', 'plugin-smoke') == 'fresh-interrupted-review':
            env = {**(env or {}), 'PLANAR_HOME': '/workspace/home'}
        is_exec = 'codex exec ' in command
        if is_exec:
            self._record('planar-launch.json', {'command': command, 'codex_home': HOME, 'model': self.model_name, 'cli_version': self.version(), 'status': 'started'})
        try:
            result = await super().exec_as_agent(environment, command, env=env, cwd=cwd, timeout_sec=timeout_sec)
        except BaseException:
            if is_exec:
                self._record('planar-terminal.json', {'status': 'error_or_cancelled'})
            raise
        if is_exec:
            self._record('planar-terminal.json', {'status': 'shell_completed', 'return_code': result.return_code})
        return result

    async def run(self, instruction, environment, context):
        if getattr(self, '_case_name', 'plugin-smoke') == 'fresh-interrupted-review':
            validate_recovery_selection(instruction, self.model_name, self.options.reasoning_effort)
        if self._resolve_auth_json_path() is None:
            raise ValueError('file-backed subscription authentication is required')
        approved_effort = os.environ.get('PLANAR_T044_EFFORT', 'medium')
        if approved_effort not in ('medium', 'high') or self.version() != '0.169.0' or self.model_name != 'gpt-6.1-terra' or self.options.reasoning_effort != approved_effort:
            raise ValueError('unexpected CLI version or model')
        if self._resume or self._load or self.mcp_servers or self.skills_dir:
            raise ValueError('only fresh, unextended exec is supported')
        try:
            return await super().run(instruction, environment, context)
        finally:
            # Root removes fixed paths even if the native setup failed before its
            # own finally. Treat inability to verify cleanup as an infrastructure error.
            async def cleanup():
                await self.exec_as_root(environment, command='rm -rf /tmp/codex-secrets /installed-agent/codex-home && test ! -e /tmp/codex-secrets && test ! -e /installed-agent/codex-home', timeout_sec=30)
            async def bounded_cleanup():
                async with asyncio.timeout(35):
                    await cleanup()
            task = asyncio.create_task(bounded_cleanup())
            cancelled = False
            try:
                try:
                    await asyncio.shield(task)
                except asyncio.CancelledError:
                    cancelled = True
                    await task
            except BaseException:
                self._record('planar-cleanup.json', {'status': 'failed', 'paths': ['/tmp/codex-secrets', HOME]})
                raise
            else:
                self._record('planar-cleanup.json', {'status': 'passed', 'paths': ['/tmp/codex-secrets', HOME]})
            if cancelled:
                raise asyncio.CancelledError
