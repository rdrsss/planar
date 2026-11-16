import asyncio
import tempfile
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, patch

from harbor.agents.installed.codex import Codex
from smoke_codex import HOME, SmokeCodex


class CleanupTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.agent = SmokeCodex.__new__(SmokeCodex)
        self.agent.logs_dir = Path(self.tmp.name)
        self.agent._provenance_dir = Path(self.tmp.name)
        self.agent._execution_id = 'test-execution'
        self.agent._version = '0.169.0'
        self.agent.model_name = 'gpt-6.1-terra'
        from types import SimpleNamespace
        self.agent.options = SimpleNamespace(reasoning_effort='medium')
        self.agent._resume = False
        self.agent._load = False
        self.agent.mcp_servers = []
        self.agent.skills_dir = None
        self.agent._resolve_auth_json_path = lambda: Path('/not-read/auth.json')
        self.agent.exec_as_root = AsyncMock()

    async def test_setup_failure_cleans_fixed_paths(self):
        with patch.object(Codex, 'run', new=AsyncMock(side_effect=RuntimeError('hook failure'))):
            with self.assertRaisesRegex(RuntimeError, 'hook failure'):
                await self.agent.run('test', object(), object())
        self.assertIn('/tmp/codex-secrets', self.agent.exec_as_root.call_args.kwargs['command'])
        self.assertIn(HOME, self.agent.exec_as_root.call_args.kwargs['command'])
        self.assertIn('passed', (self.agent.logs_dir / 'planar-cleanup.json').read_text())

    async def test_cancellation_waits_for_cleanup(self):
        started = asyncio.Event()
        async def pending(*args, **kwargs):
            started.set()
            await asyncio.Event().wait()
        with patch.object(Codex, 'run', new=pending):
            task = asyncio.create_task(self.agent.run('test', object(), object()))
            await asyncio.wait_for(started.wait(), 3)
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
        self.agent.exec_as_root.assert_awaited_once()
        self.assertIn('passed', (self.agent.logs_dir / 'planar-cleanup.json').read_text())

    async def test_timeout_still_cleans(self):
        async def pending(*args, **kwargs):
            await asyncio.Event().wait()
        with patch.object(Codex, 'run', new=pending):
            with self.assertRaises(TimeoutError):
                await asyncio.wait_for(self.agent.run('test', object(), object()), 0.02)
        self.agent.exec_as_root.assert_awaited_once()
        self.assertIn('passed', (self.agent.logs_dir / 'planar-cleanup.json').read_text())

    async def test_cleanup_failure_is_not_suppressed(self):
        self.agent.exec_as_root.side_effect = RuntimeError('cleanup failed')
        with patch.object(Codex, 'run', new=AsyncMock()):
            with self.assertRaisesRegex(RuntimeError, 'cleanup failed'):
                await self.agent.run('test', object(), object())
        self.assertIn('failed', (self.agent.logs_dir / 'planar-cleanup.json').read_text())

class OrderingTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        import json
        from types import SimpleNamespace
        from smoke_codex import CACHE, CONFIG, MANIFEST
        self.json = json
        self.config = CONFIG
        self.cache = CACHE
        self.manifest = json.loads(MANIFEST.read_text())
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.agent = SmokeCodex.__new__(SmokeCodex)
        self.agent.logs_dir = Path(self.tmp.name)
        self.agent._provenance_dir = Path(self.tmp.name)
        self.agent._execution_id = 'test-execution'
        self.agent.mcp_servers = []
        self.agent.skills_dir = None
        self.agent._resume = False
        self.agent._load = False
        self.events = []
        self.environment = SimpleNamespace(upload_dir=AsyncMock(), upload_file=AsyncMock())
        self.outputs = [
            'codex-cli 0.169.0', '', '',
            json.dumps({'pluginId': 'planar@planar-smoke-market', 'installedPath': CACHE}),
            json.dumps(['planar@planar-smoke-market']), json.dumps(self.manifest),
            json.dumps({**CONFIG, 'plugins': {**CONFIG['plugins'], 'planar@planar-smoke-market': {'enabled': True}}}),
        ]
        async def execute(environment, command, env=None):
            self.events.append(command)
            return SimpleNamespace(stdout=self.outputs.pop(0), return_code=0)
        self.agent.exec_as_agent = execute
        self.agent._image_identity = lambda environment: {'name': 'test-image', 'id': 'sha256:' + 'a' * 64}
        self.agent._ensure_agent_owned_dir = AsyncMock(return_value=(1000, 1000))
        self.agent.exec_as_root = AsyncMock()

    async def test_base_install_enable_and_cache_check_order(self):
        async def upload(*args):
            self.events.append('base upload')
        with patch.object(Codex, '_upload_effective_config', new=upload):
            await self.agent._upload_effective_config(self.environment, self.config, HOME + '/config.toml')
        self.assertEqual(self.events[0], 'base upload')
        self.assertIn('codex --version', self.events[1])
        self.assertIn('/home/agent', self.events[2])
        self.assertIn('marketplace add', self.events[3])
        self.assertIn('plugin add', self.events[4])
        self.assertIn('plugin list', self.events[5])
        self.assertIn('planar-inventory.py', self.events[6])
        self.assertIn('config.toml', self.events[7])
        self.assertTrue((self.agent.logs_dir / 'planar-install-provenance.json').is_file())

    async def test_install_failure_stops_hook(self):
        async def upload(*args):
            self.events.append('base upload')
        async def failure(*args, **kwargs):
            self.events.append('install failed')
            raise RuntimeError('installation failure')
        self.agent.exec_as_agent = failure
        with patch.object(Codex, '_upload_effective_config', new=upload):
            with self.assertRaisesRegex(RuntimeError, 'installation failure'):
                await self.agent._upload_effective_config(self.environment, self.config, HOME + '/config.toml')
        self.assertEqual(self.events, ['base upload', 'install failed'])
        self.assertFalse((self.agent.logs_dir / 'planar-install-provenance.json').exists())
