"""Harbor Docker environment with per-run project identity and export denial.

This is a fail-closed integration step. It deliberately denies all container
exports until a bounded archive transport is integrated and reviewed.
"""
import asyncio
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from harbor.models.task.config import NetworkMode

from harbor.environments.docker.docker import DockerEnvironment, _sanitize_docker_compose_project_name

from safe_export import MAX_ARCHIVE, select_archive, select_flat_archive, select_native_logs, write_tree


class GuardedDocker(DockerEnvironment):
    def __init__(self, *args, **kwargs):
        evidence = Path(os.environ['PLANAR_T044_EVIDENCE_DIR']).resolve(strict=True)
        run_id = evidence.name
        if not re.fullmatch(r'planar-t044-(?:noauth-)?[0-9a-f]{32}', run_id):
            raise ValueError('invalid launcher execution identity')
        raw_session = kwargs.get('session_id')
        if not isinstance(raw_session, str):
            raise ValueError('environment session identity is required')
        self._static_verifier = '__verifier__' in raw_session
        if self._static_verifier:
            expected = Path(__file__).resolve().parents[1] / 'cases/plugin-smoke/tests'
            if Path(kwargs['environment_dir']).resolve() != expected.resolve():
                raise ValueError('unexpected verifier environment directory')
            policy = kwargs.get('network_policy')
            if policy is None or policy.network_mode != NetworkMode.NO_NETWORK:
                raise ValueError('verifier requires a no-network policy')
        kwargs['session_id'] = run_id + '-' + raw_session
        super().__init__(*args, **kwargs)
        if self._static_verifier:
            self._enable_egress_control = False
        if self.auto_mounting or self._host_mounts:
            raise ValueError('host mounts are forbidden for diagnostic')
        project = _sanitize_docker_compose_project_name(self.session_id)
        registry = evidence / 'projects'
        registry.mkdir(mode=0o700, exist_ok=True)
        path = registry / (hashlib.sha256(project.encode()).hexdigest() + '.json')
        data = {'execution_id': run_id, 'compose_project': project}
        # Register before Docker start, including for separate verifier projects.
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as file:
            json.dump(data, file)
            file.flush()
            os.fsync(file.fileno())

    @property
    def capabilities(self):
        capabilities = super().capabilities
        if self._static_verifier:
            return capabilities.model_copy(update={'disable_internet': True, 'dynamic_network_policy': False})
        return capabilities

    async def start(self, force_build):
        await super().start(force_build)
        if self._static_verifier:
            cid = await self._platform._resolve_service_container('main')
            result = subprocess.run(['docker', 'inspect', '--format', '{{.HostConfig.NetworkMode}}', cid], capture_output=True, text=True, check=True, timeout=15)
            if result.stdout.strip() != 'none':
                raise RuntimeError('verifier container is not network-disabled')
            evidence = Path(os.environ['PLANAR_T044_EVIDENCE_DIR']).resolve(strict=True)
            (evidence / 'verifier-network.json').write_text(json.dumps({'execution_id': evidence.name, 'container_id': cid, 'network_mode': 'none'}))

    async def _apply_network_policy(self, network_policy):
        if self._static_verifier and network_policy.network_mode == NetworkMode.NO_NETWORK:
            return
        await super()._apply_network_policy(network_policy)

    async def _archive(self, source):
        cid = await self._platform._resolve_service_container('main')
        process = await asyncio.create_subprocess_exec('docker', 'cp', f'{cid}:{source}', '-', stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
        async def collect():
            chunks = []
            total = 0
            while True:
                chunk = await process.stdout.read(min(65536, MAX_ARCHIVE + 1 - total))
                if not chunk:
                    break
                total += len(chunk)
                if total > MAX_ARCHIVE:
                    raise ValueError('container export exceeds archive limit')
                chunks.append(chunk)
            code = await process.wait()
            if code:
                raise RuntimeError('Docker archive export failed')
            return b''.join(chunks)
        try:
            return await asyncio.wait_for(collect(), timeout=30)
        finally:
            if process.returncode is None:
                process.kill()
                await process.wait()

    async def download_dir(self, source_dir, target_dir):
        if source_dir == '/logs/agent' and not self._static_verifier:
            archive = await self._archive(source_dir + '/.')
            write_tree(Path(target_dir), select_native_logs(archive))
        elif source_dir == '/logs/artifacts' and not self._static_verifier:
            archive = await self._archive(source_dir + '/.')
            write_tree(Path(target_dir), select_flat_archive(archive, set()))
        elif source_dir == '/logs/verifier' and self._static_verifier:
            archive = await self._archive(source_dir + '/.')
            write_tree(Path(target_dir), select_flat_archive(archive, {'reward.txt', 'status.json', 'test-stdout.txt'}))
        else:
            raise RuntimeError('container directory export denied')

    async def download_file(self, source_path, target_path):
        if source_path != '/workspace/RESULT.txt':
            raise RuntimeError('container file export denied')
        cid = await self._platform._resolve_service_container('main')
        check = await asyncio.create_subprocess_exec('docker', 'exec', '--user', 'root', cid, 'python3', '-c', 'import os,stat; p="/workspace"; e=os.listdir(p); assert e == ["RESULT.txt"] and stat.S_ISREG(os.lstat(p+"/RESULT.txt").st_mode)', stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL)
        try:
            status = await asyncio.wait_for(check.wait(), timeout=15)
        finally:
            if check.returncode is None:
                check.kill()
                await check.wait()
        if status:
            raise ValueError('fixture contains unexpected entries or invalid result')
        archive = await self._archive(source_path)
        selected = select_archive(archive, {'RESULT.txt'})
        if set(selected) != {'RESULT.txt'}:
            raise ValueError('expected result is absent')
        target = Path(target_path)
        if target.name != 'RESULT.txt':
            raise ValueError('unexpected output filename')
        write_tree(target.parent, selected)

    async def service_download_dir(self, source_dir, target_dir, service=None):
        if service not in (None, 'main'):
            raise RuntimeError('sidecar export denied')
        return await self.download_dir(source_dir, target_dir)

    async def service_download_file(self, source_path, target_path, service=None):
        if service not in (None, 'main'):
            raise RuntimeError('sidecar export denied')
        return await self.download_file(source_path, target_path)

    async def service_download_dir_with_exclusions(self, *args, **kwargs):
        raise RuntimeError('filtered export unsupported')

    async def download_dir_filtered(self, *args, **kwargs):
        raise RuntimeError('filtered export unsupported')

    async def download_dir_with_exclusions(self, *args, **kwargs):
        raise RuntimeError('filtered export unsupported')
