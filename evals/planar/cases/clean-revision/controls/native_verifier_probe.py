"""No-model native separate-verifier control; writes only to a new temp root."""
import asyncio
import json
import os
import subprocess
import sys
from pathlib import Path
import tempfile
from harbor.agents.nop import NopAgent
from harbor.environments.docker.docker import DockerEnvironment
from harbor.job import Job
from harbor.models.task.config import NetworkMode
from harbor.models.job.config import JobConfig

class ControlAgent(NopAgent):
    async def run(self, instruction, environment, context):
        if instruction.strip() not in ('first', 'second'):
            raise ValueError('unexpected control instruction')
        await super().run(instruction, environment, context)

class ControlDocker(DockerEnvironment):
    def __init__(self, *args, **kwargs):
        self._control_verifier = '__verifier__' in kwargs.get('session_id', '')
        super().__init__(*args, **kwargs)
    @property
    def capabilities(self):
        value = super().capabilities
        return value.model_copy(update={'disable_internet': True, 'dynamic_network_policy': False}) if self._control_verifier else value
    async def _apply_network_policy(self, policy):
        if self._control_verifier and policy.network_mode == NetworkMode.NO_NETWORK:
            return
        await super()._apply_network_policy(policy)
    async def start(self, force_build=False):
        await super().start(force_build)
        cid = await self._platform._resolve_service_container('main')
        info = json.loads(subprocess.check_output(['docker', 'inspect', cid], text=True))[0]
        mode = info['HostConfig']['NetworkMode']
        project = info['Config']['Labels'].get('com.docker.compose.project')
        with Path(os.environ['PLANAR_CONTROL_TRACE']).open('a') as trace:
            trace.write(json.dumps({'container_id': cid, 'compose_project': project, 'verifier': self._control_verifier, 'network_mode': mode})+'\n')
        if self._control_verifier and mode != 'none':
            raise ValueError('verifier network is not none')

async def run():
    for name in ('OPENAI_API_KEY', 'ANTHROPIC_API_KEY', 'CODEX_API_KEY', 'CODEX_AUTH_JSON_PATH', 'CODEX_FORCE_AUTH_JSON'):
        os.environ.pop(name, None)
    fail_a = sys.argv[1:] == ['--fail-a']
    if sys.argv[1:] not in ([], ['--fail-a']): raise ValueError('unsupported control variant')
    root = Path(tempfile.mkdtemp(prefix='planar-native-verifier-'))
    os.environ['PLANAR_CONTROL_TRACE'] = str(root/'environment-trace.jsonl')
    case = root/'case'; case.mkdir()
    image = 'sha256:80c72c37a539e081165e5cebf0f4a6e33852e34df7cee7afaefc3bc0ab0da5a7'
    (case/'task.toml').write_text(f'''schema_version = "1.4"
multi_step_reward_strategy = "final"
[task]
name = "planar/separate-verifier-control"
description = "No-model artifact handoff control"
[environment]
docker_image = "{image}"
cpus = 1
memory_mb = 1024
storage_mb = 1024
[agent]
timeout_sec = 30
[verifier]
environment_mode = "separate"
timeout_sec = 30
[verifier.environment]
network_mode = "no-network"
cpus = 1
memory_mb = 1024
storage_mb = 1024
[[steps]]
name = "a"
min_reward = 1
artifacts = ["/workspace/probe-marker"]
[[steps]]
name = "b"
artifacts = ["/workspace/probe-marker"]
''')
    for name, instruction, body, expected in (
        ('a','first',"printf 'preserved\\n' > /workspace/probe-marker", 'preserved'),
        ('b','second',"test \"$(cat /workspace/probe-marker)\" = preserved && printf 'stage-b\\n' >> /workspace/probe-marker", 'stage-b')):
        step = case/'steps'/name
        (step/'workdir').mkdir(parents=True)
        (step/'tests').mkdir()
        (step/'instruction.md').write_text(instruction+'\n')
        (step/'workdir/setup.sh').write_text('#!/bin/sh\nset -eu\nmkdir -p /workspace\n'+body+'\n')
        reward = '0' if fail_a and name == 'a' else '1'
        (step/'tests/test.sh').write_text('#!/bin/sh\nset -eu\nmkdir -p /logs/verifier\ngrep -qx '+expected+' /workspace/probe-marker\nprintf '+reward+' > /logs/verifier/reward.txt\n')
    (case/'environment').mkdir()
    (case/'environment/docker-compose.yaml').write_text('services:\n  main:\n    network_mode: none\n')
    (case/'instruction.md').write_text('control\n')
    config = JobConfig.model_validate({'job_name':'native-separate-verifier-control', 'jobs_dir':str(root/'jobs'), 'n_attempts':1, 'n_concurrent_trials':1, 'retry':{'max_retries':0}, 'agents':[{'import_path':'native_verifier_probe:ControlAgent', 'resume_trajectory':False}], 'environment':{'import_path':'native_verifier_probe:ControlDocker', 'delete':True}, 'tasks':[{'path':str(case)}], 'quiet':True})
    (root/'config.json').write_text(config.model_dump_json(indent=2))
    job = await Job.create(config)
    result = await asyncio.wait_for(job.run(), timeout=180)
    (root/'result.json').write_text(result.model_dump_json(indent=2))
    observed = [json.loads(line) for line in (root/'environment-trace.jsonl').read_text().splitlines()]
    checks = {}
    for item in observed:
        project = item['compose_project']
        if not project: raise RuntimeError('compose identity unavailable')
        checks[project] = {}
        for kind in ('container', 'network', 'volume'):
            output = subprocess.check_output(['docker', kind, 'ls', '-aq' if kind == 'container' else '-q', '--filter', 'label=com.docker.compose.project='+project], text=True)
            checks[project][kind] = output.splitlines()
    (root/'observations.json').write_text(json.dumps({'environments':observed, 'cleanup':checks}, indent=2)+'\n')
    if len([x for x in observed if x['verifier']]) != (1 if fail_a else 2) or any(ids for projects in checks.values() for ids in projects.values()):
        raise RuntimeError('verifier count or cleanup mismatch')
    print(str(root), flush=True)
    trials = list((root/'jobs').glob('*/*/result.json'))
    if len(trials) != 1: raise RuntimeError('missing native trial result')
    trial = json.loads(trials[0].read_text())
    steps = trial.get('step_results') or []
    expected_rewards = [0] if fail_a else [1, 1]
    actual_rewards = [(s.get('verifier_result') or {}).get('rewards',{}).get('reward') for s in steps]
    if actual_rewards != expected_rewards:
        raise RuntimeError('separate verifier or A gate failed; see '+str(trials[0]))
    print('first-step gate stopped B' if fail_a else 'two separate verifier rewards passed', flush=True)

if __name__ == '__main__': asyncio.run(run())
