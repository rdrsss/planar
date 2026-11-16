"""Run one no-model Harbor job in a test-only fake environment.

No authentication, external process or real verifier is used. This entry point
is deliberately limited to nop and cannot launch Codex.
"""
import asyncio
import os
import sys
from pathlib import Path

from harbor.job import Job
from harbor.models.job.config import JobConfig
from controls import create, read


async def run(folder):
    reservation = read(folder / 'start.json')
    manifest = read(folder.parent.parent / 'manifest.json')
    config = dict(manifest['config'])
    config['tasks'] = [{'path': str(folder.parent.parent / 'case-snapshot')}]
    config.update(job_name=reservation['native_job_name'], jobs_dir=str(folder),
                  n_attempts=1, n_concurrent_trials=1, retry={'max_retries': 0},
                  agents=[{'name': 'nop'}],
                  environment={'import_path': 'fake_environment:FakeEnvironment'},
                  verifier={'disable': True}, quiet=True)
    job = await Job.create(JobConfig.model_validate(config))

    async def started(event):
        # Harbor fires START before environment setup and host dispatch.
        create(folder / 'native-start.json', {
            'job_id': str(event.config.job_id), 'trial_id': str(event.trial_id),
            'trial_name': event.trial_name,
            'raw_job': str(job.job_dir), 'injected_environment': True})

    job.on_trial_started(started)
    result = await job.run()
    # Native output is retained as-is; no verifier means there is no pass.
    create(folder / 'native-complete.json', {'job_id': str(result.id),
           'n_total_trials': result.n_total_trials, 'grade': 'unavailable'})


if __name__ == '__main__':
    # Harbor progress belongs in neither the JSON CLI output nor the evidence.
    with open(os.devnull, 'w') as sink:
        os.dup2(sink.fileno(), 1)
    asyncio.run(run(Path(sys.argv[1])))
