"""Independent diagnostic supervisor; cleanup remains required on its death."""
import json
import os
from pathlib import Path
import sys

from recover import recover_projects
from supervise import supervise


def main():
    evidence=Path(os.environ['PLANAR_T044_EVIDENCE_DIR']).resolve(strict=True)
    parent=int(os.environ['PLANAR_T044_PARENT_PID'])
    marker=evidence/'recovery-required'
    if not marker.is_file():
        raise RuntimeError('durable recovery marker missing')
    command=[str(Path(sys.executable).parent/'harbor'), 'run', '--config', str(evidence/'job-config.json')]
    def on_start(pid):
        (evidence/'harbor-process.json').write_text(json.dumps({'pid':pid,'execution_id':evidence.name}))
    def cleanup():
        recover_projects(evidence)
        marker.unlink()
    try:
        status=supervise(command,deadline=1500,grace=15,cleanup=cleanup,parent_pid=parent,on_start=on_start)
    except BaseException:
        (evidence/'watchdog-terminal.json').write_text(json.dumps({'execution_id':evidence.name,'status':'cleanup_unconfirmed'}))
        raise
    (evidence/'watchdog-terminal.json').write_text(json.dumps({'execution_id':evidence.name,**status}))
    return 2

if __name__ == '__main__': sys.exit(main())
