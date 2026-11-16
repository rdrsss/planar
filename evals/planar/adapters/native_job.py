"""Harbor job entry point with durable pre-environment native identity."""
from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path
import sys

from harbor.job import Job
from harbor.models.job.config import JobConfig


async def run(config_path, evidence):
    config = JobConfig.model_validate_json(Path(config_path).read_text())
    if (config.n_attempts != 1 or config.n_concurrent_trials != 1 or config.retry.max_retries != 0
        or len(config.agents) != 1 or len(config.tasks) != 1 or config.datasets):
        raise ValueError('only one fresh serial trial with zero retries is permitted')
    evidence = Path(evidence).resolve(strict=True)
    if config.jobs_dir.resolve(strict=True) != evidence or config.job_name != evidence.name:
        raise ValueError('job must use its new launcher evidence directory')
    if (evidence / config.job_name).exists() or (evidence / config.job_name).is_symlink():
        raise ValueError('native job directory already exists; resume is forbidden')
    job = await Job.create(config)

    async def started(event):
        # Harbor invokes START before preparing its environment or running agent.
        path = Path(evidence) / 'native-trial-start.json'
        record = {'job_id': str(event.config.job_id), 'trial_id': str(event.trial_id),
                  'trial_name': event.trial_name, 'job_dir': str(job.job_dir)}
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as output:
            json.dump(record, output, sort_keys=True)
            output.flush()
            os.fsync(output.fileno())
        directory = os.open(evidence, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)

    job.on_trial_started(started)
    await job.run()


if __name__ == '__main__':
    asyncio.run(run(sys.argv[1], sys.argv[2]))
