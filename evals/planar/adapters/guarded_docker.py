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
import shutil
import subprocess
import sys

from harbor.models.task.config import NetworkMode

from harbor.environments.docker.docker import DockerEnvironment, _sanitize_docker_compose_project_name

from safe_export import MAX_ARCHIVE, select_archive, select_flat_archive, select_native_logs, select_recovery_home, write_tree


class GuardedDocker(DockerEnvironment):
    def __init__(self, *args, **kwargs):
        evidence = Path(os.environ['PLANAR_T044_EVIDENCE_DIR']).resolve(strict=True)
        run_id = evidence.name
        if not re.fullmatch(r'planar-t044-(?:noauth-)?[0-9a-f]{32}', run_id):
            raise ValueError('invalid launcher execution identity')
        case_name = os.environ.get('PLANAR_T044_CASE', 'plugin-smoke')
        if case_name not in ('plugin-smoke', 'native-lineage-smoke', 'native-lineage-current', 'fresh-interrupted-review', 'clean-revision', 'native-lineage-portable', 'retained-a-fresh-b'):
            raise ValueError('unsupported diagnostic case')
        if case_name == 'retained-a-fresh-b':
            if os.environ.get('PLANAR_T045_NOAUTH_CONTROL') == '1':
                if any(os.environ.get(key) for key in ('CODEX_AUTH_JSON_PATH','CODEX_FORCE_AUTH_JSON','OPENAI_API_KEY','CODEX_API_KEY')):
                    raise ValueError('retained noauth control cannot carry credentials')
            else:
                if any(not os.environ.get(key) for key in ('PLANAR_T044_CASE_SNAPSHOT','PLANAR_T045_SOURCE_PACKET','PLANAR_T044_CANDIDATE_SNAPSHOT')):
                    raise ValueError('retained host admission missing')
                from retained_b_support import verify_runtime
                verify_runtime(evidence,os.environ['PLANAR_T044_CASE_SNAPSHOT'],os.environ['PLANAR_T045_SOURCE_PACKET'],os.environ['PLANAR_T044_CANDIDATE_SNAPSHOT'])
        self._case_name = case_name
        self._evidence_root = evidence
        raw_session = kwargs.get('session_id')
        if not isinstance(raw_session, str):
            raise ValueError('environment session identity is required')
        self._static_verifier = '__verifier__' in raw_session
        self._verifier_step = None
        if self._static_verifier and case_name == 'clean-revision':
            match = re.search(r'__verifier__(worker|revision)$', raw_session)
            if match is None: raise ValueError('invalid clean verifier step identity')
            self._verifier_step = match.group(1)
        if self._static_verifier:
            snapshot = os.environ.get('PLANAR_T044_CASE_SNAPSHOT')
            if case_name == 'native-lineage-portable' and not snapshot:
                raise ValueError('portable verifier requires frozen case snapshot')
            if snapshot:
                from launch import case_inventory
                root = Path(snapshot)
                if not root.is_absolute() or root != root.resolve(strict=True):
                    raise ValueError('unsafe case snapshot path')
                record = json.loads((evidence / 'launcher.json').read_text())
                digest = os.environ.get('PLANAR_T044_CASE_SHA256')
                if (not os.environ.get('PLANAR_T044_CANDIDATE_SNAPSHOT') or
                    record.get('execution_id') != run_id or record.get('case') != case_name or
                    record.get('case_sha256') != digest or
                    record.get('prerequisites', {}).get('case_read_root') != str(root) or
                    case_inventory(root) != digest):
                    raise ValueError('case snapshot identity mismatch')
                expected = root / 'tests'
            else:
                expected = Path(__file__).resolve().parents[1] / 'cases' / case_name / 'tests'
            directory = Path(kwargs['environment_dir'])
            if directory != expected or directory != directory.resolve(strict=True):
                raise ValueError('unexpected verifier environment directory')
            policy = kwargs.get('network_policy')
            if policy is None or policy.network_mode != NetworkMode.NO_NETWORK:
                raise ValueError('verifier requires a no-network policy')
            if case_name == 'native-lineage-portable':
                # Only the separate verifier may substitute the independently
                # reviewed data-baked image into a deep copy of Harbor's config.
                sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'experiments'))
                from schema2_projection import effective_verifier_environment, project, exact
                image_id = os.environ.get('PLANAR_T045_VERIFIER_IMAGE_ID')
                if (record.get('runnable_contract') != 'planar-schema2-runnable-v1'
                    or record.get('verifier_image_id') != image_id):
                    raise ValueError('portable verifier binding missing')
                effective = effective_verifier_environment(kwargs['task_env_config'], image_id)
                if not exact(project(effective), record.get('effective_verifier_environment')):
                    raise ValueError('portable verifier configuration mismatch')
                kwargs['task_env_config'] = effective
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
        if self._static_verifier and self._case_name in ('native-lineage-portable','retained-a-fresh-b') and force_build:
            raise ValueError('portable verifier cannot rebuild')
        await super().start(force_build)
        if self._static_verifier:
            cid = await self._platform._resolve_service_container('main')
            result = subprocess.run(['docker', 'inspect', '--format', '{{.HostConfig.NetworkMode}}', cid], capture_output=True, text=True, check=True, timeout=15)
            if result.stdout.strip() != 'none':
                raise RuntimeError('verifier container is not network-disabled')
            if self._case_name in ('clean-revision', 'native-lineage-portable', 'retained-a-fresh-b'):
                expected = os.environ.get('PLANAR_T045_VERIFIER_IMAGE_ID')
                noauth = os.environ.get('PLANAR_T045_NOAUTH_CONTROL') == '1'
                if noauth and any(os.environ.get(key) for key in ('CODEX_AUTH_JSON_PATH','CODEX_FORCE_AUTH_JSON','OPENAI_API_KEY','CODEX_API_KEY')):
                    raise ValueError('no-auth control cannot carry authentication')
                if not expected and not noauth: raise ValueError('reviewed verifier image identity missing')
                actual = subprocess.run(['docker','inspect','--format','{{.Image}}',cid], capture_output=True, text=True, check=True, timeout=15).stdout.strip()
                if expected and actual != expected: raise ValueError('actual verifier image differs from gate')
            evidence = Path(os.environ['PLANAR_T044_EVIDENCE_DIR']).resolve(strict=True)
            name = 'verifier-network-'+self._verifier_step+'.json' if self._verifier_step else 'verifier-network.json'
            record = {'execution_id': evidence.name, 'container_id': cid, 'network_mode': 'none'}
            if self._case_name in ('native-lineage-portable','retained-a-fresh-b'):
                record['image_id'] = actual
            if self._verifier_step:
                record['step'] = self._verifier_step
                record['image_id'] = actual
            (evidence / name).write_text(json.dumps(record))

    async def _run_docker_compose_command(self, command, check=True, timeout_sec=None, stdin_data=None, on_output=None):
        if self._static_verifier and self._case_name in ('native-lineage-portable','retained-a-fresh-b'):
            if command and command[0] in ('build', 'pull'):
                raise ValueError('portable verifier cannot build or pull at runtime')
            if command and command[0] == 'up':
                if any(arg in ('--build', '--pull') or arg.startswith('--pull=') for arg in command[1:]):
                    raise ValueError('portable verifier has conflicting compose flags')
                command = [*command, '--no-build', '--pull', 'never']
                path = self._evidence_root / 'verifier-compose-up.json'
                fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, 'w') as output:
                    json.dump({'execution_id': self._evidence_root.name, 'command': command}, output)
                    output.flush(); os.fsync(output.fileno())
        return await super()._run_docker_compose_command(command, check=check,
            timeout_sec=timeout_sec, stdin_data=stdin_data, on_output=on_output)

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
            logs = select_native_logs(archive)
            write_tree(Path(target_dir), logs)
            if self._case_name == 'retained-a-fresh-b':
                destination=self._evidence_root/'retained-b/logs/revision'
                if destination.exists() or destination.is_symlink(): raise ValueError('target logs already collected')
                write_tree(destination,logs)
            if self._case_name == 'clean-revision':
                clean = self._evidence_root/'clean-revision'
                active = json.loads((clean/'active.json').read_text())
                step = active.get('step')
                if step not in ('worker', 'revision'): raise ValueError('invalid clean step')
                destination = clean/'logs'/step
                if destination.exists() or destination.is_symlink(): raise ValueError('clean step logs already collected')
                write_tree(destination, logs)
        elif source_dir == '/workspace/home' and not self._static_verifier and self._case_name == 'retained-a-fresh-b':
            from clean_revision_support import load_case_module
            from retained_b_support import source as validate_source
            from candidate import inventory
            case=Path(os.environ['PLANAR_T044_CASE_SNAPSHOT']); candidate=Path(os.environ['PLANAR_T044_CANDIDATE_SNAPSHOT'])
            packet=Path(os.environ['PLANAR_T045_SOURCE_PACKET'])
            binding=validate_source(case,packet,candidate)
            if binding['source_packet_sha256'] != os.environ.get('PLANAR_T045_SOURCE_SHA256'):
                raise ValueError('retained source identity changed before verifier handoff')
            expected=json.loads((case/'candidate.json').read_text())
            if inventory(candidate)!=expected: raise ValueError('candidate changed before verifier handoff')
            capture=load_case_module(case,'capture'); target=self._evidence_root/'retained-b'
            identity=json.loads((target/'container.json').read_text())
            cid=await self._platform._resolve_service_container('main')
            actual=subprocess.run(['docker','inspect','--format','{{.Id}}',cid],check=True,capture_output=True,text=True,timeout=15).stdout.strip()
            if actual!=identity.get('container_id'): raise ValueError('retained target container identity changed')
            if not (target/'logs/revision').is_dir(): raise ValueError('retained target logs missing')
            snapshot=await capture.collect(self,actual,identity.get('baseline'),failure_evidence=target)
            capture.save(target/'final',snapshot)
            home={name[5:]:value for name,value in snapshot.items() if name.startswith('home/')}
            write_tree(Path(target_dir),home)
            host=Path(target_dir)/'_planar_host'
            if host.exists() or host.is_symlink(): raise ValueError('host evidence destination exists')
            host.mkdir(mode=0o700)
            shutil.copytree(packet,host/'source-packet',symlinks=True)
            shutil.copytree(candidate,host/'candidate',symlinks=True)
            shutil.copytree(target,host/'target',symlinks=True,ignore=shutil.ignore_patterns('rejected-home.tar','rejected-home.json'))
            for item in (host,*host.rglob('*')):
                if item.is_symlink(): raise ValueError('symlink in retained verifier input')
                if item.is_dir(): item.chmod(0o755)
                elif item.is_file(): item.chmod(0o644)
                else: raise ValueError('special retained verifier input')
        elif source_dir == '/workspace/home' and not self._static_verifier and self._case_name == 'clean-revision':
            from clean_revision_support import load_case_module
            case = Path(os.environ.get('PLANAR_T044_CASE_SNAPSHOT') or Path(__file__).resolve().parents[1]/'cases/clean-revision')
            capture = load_case_module(case, 'capture')
            clean = self._evidence_root/'clean-revision'
            active = json.loads((clean/'active.json').read_text())
            step = active.get('step')
            if step not in ('worker', 'revision'): raise ValueError('invalid clean step')
            identity = json.loads((clean/'container.json').read_text())
            cid = await self._platform._resolve_service_container('main')
            actual = subprocess.run(['docker','inspect','--format','{{.Id}}',cid], check=True, capture_output=True, text=True, timeout=15).stdout.strip()
            if actual != identity.get('container_id'): raise ValueError('clean container identity changed')
            if not (clean/'logs'/step).is_dir(): raise ValueError('clean native logs missing')
            snapshot = await capture.collect(self, actual, identity.get('baseline'), failure_evidence=clean)
            capture.save(clean/('pre' if step == 'worker' else 'final'), snapshot)
            home = {name[5:]: value for name,value in snapshot.items() if name.startswith('home/')}
            write_tree(Path(target_dir), home)
            host = Path(target_dir)/'_planar_host'
            if host.exists() or host.is_symlink(): raise ValueError('host evidence destination exists')
            shutil.copytree(clean, host, symlinks=True, ignore=shutil.ignore_patterns('rejected-home.tar','rejected-home.json'))
            # Only this exported verifier copy is made readable by the separate
            # container user; the host-owned originals stay private.
            for item in (host, *host.rglob('*')):
                if item.is_symlink(): raise ValueError('symlink in host evidence')
                if item.is_dir(): item.chmod(0o755)
                elif item.is_file(): item.chmod(0o644)
                else: raise ValueError('special host evidence entry')
        elif source_dir == '/workspace/home' and not self._static_verifier and self._case_name == 'fresh-interrupted-review':
            fixture = Path(__file__).resolve().parents[1] / 'cases/fresh-interrupted-review/fixture/inventory.json'
            manifest = json.loads(fixture.read_text())['files']
            retained = {name[len('home/'):] for name in manifest if name.startswith('home/')}
            archive = await self._archive(source_dir + '/.')
            write_tree(Path(target_dir), select_recovery_home(archive, retained))
        elif source_dir == '/logs/artifacts' and not self._static_verifier:
            archive = await self._archive(source_dir + '/.')
            write_tree(Path(target_dir), select_flat_archive(archive, set()))
        elif source_dir == '/logs/verifier' and self._static_verifier:
            archive = await self._archive(source_dir + '/.')
            write_tree(Path(target_dir), select_flat_archive(archive, {'reward.txt', 'status.json', 'test-stdout.txt'}))
        else:
            raise RuntimeError('container directory export denied')

    async def download_file(self, source_path, target_path):
        if self._case_name == 'fresh-interrupted-review':
            allowed = {'/workspace/repository/README.md', '/workspace/repository/RESULT.txt', '/workspace/repository/STATE.txt'}
            if source_path not in allowed: raise RuntimeError('container file export denied')
            name = Path(source_path).name
            if Path(target_path).name != name: raise ValueError('unexpected output filename')
            selected = select_archive(await self._archive(source_path), {name})
            if set(selected) != {name}: raise ValueError('expected output is absent')
            write_tree(Path(target_path).parent, selected)
            return
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
