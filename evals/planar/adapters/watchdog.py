"""Independent diagnostic supervisor; cleanup remains required on its death."""
import json
import os
from pathlib import Path
import sys
import time
from datetime import datetime, timezone

from recover import recover_projects
from supervise import supervise


def process_record(evidence, pid, deadline):
    record = {'pid': pid, 'execution_id': evidence.name}
    if os.environ.get('PLANAR_T044_CASE') in ('clean-revision', 'clean-work-review', 'retained-a-fresh-b'):
        record.update({'started_monotonic_ns': time.monotonic_ns(), 'deadline_seconds': deadline,
                       'absolute_deadline': os.environ.get('PLANAR_T044_ABSOLUTE_DEADLINE')})
    return record


def main():
    evidence=Path(os.environ['PLANAR_T044_EVIDENCE_DIR']).resolve(strict=True)
    parent=int(os.environ['PLANAR_T044_PARENT_PID'])
    marker=evidence/'recovery-required'
    if not marker.is_file():
        raise RuntimeError('durable recovery marker missing')
    command=[sys.executable, str(Path(__file__).with_name('native_job.py')), str(evidence/'job-config.json'), str(evidence)]
    def on_start(pid):
        record = process_record(evidence, pid, deadline)
        (evidence/'harbor-process.json').write_text(json.dumps(record))
    def cleanup():
        recover_projects(evidence)
        marker.unlink()
    deadline=int(os.environ.get('PLANAR_T044_DEADLINE_SECONDS', '1500'))
    if not 1 <= deadline <= 1500:
        raise ValueError('invalid reviewed deadline')
    absolute = os.environ.get('PLANAR_T044_ABSOLUTE_DEADLINE')
    if absolute:
        remaining = (datetime.fromisoformat(absolute) - datetime.now(timezone.utc)).total_seconds()
        if remaining <= 0:
            cleanup()
            (evidence/'watchdog-terminal.json').write_text(json.dumps({'execution_id':evidence.name,'status':'interrupted','reason':'absolute_deadline'}))
            return 2
        deadline = min(deadline, remaining)
    try:
        status=supervise(command,deadline=deadline,grace=15,cleanup=cleanup,parent_pid=parent,on_start=on_start)
    except BaseException:
        (evidence/'watchdog-terminal.json').write_text(json.dumps({'execution_id':evidence.name,'status':'cleanup_unconfirmed'}))
        raise
    (evidence/'watchdog-terminal.json').write_text(json.dumps({'execution_id':evidence.name,**status}))
    return 2

if __name__ == '__main__': sys.exit(main())
