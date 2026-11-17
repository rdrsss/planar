"""Fresh cleanup observations for clean-work; never publishes an account marker."""
import json
import os
from datetime import datetime,timezone
from pathlib import Path

import launch
import retained_b_census
from clean_work_resources import check_projects
from retained_b_early_evidence import read_safe
from terminal_failure_schema import unique


def check(evidence, trial_name, trial):
    evidence=Path(evidence); eid=evidence.name
    reviews=launch.reviews_root().resolve(strict=True)
    if evidence.is_symlink() or evidence.resolve(strict=True).parent != launch.jobs_root().resolve(strict=True):
        raise ValueError('unsafe evidence directory')
    if (evidence/'recovery-required').exists() or (evidence/'recovery-required').is_symlink():
        raise ValueError('recovery pending')
    def read(name):
        return json.loads(read_safe(evidence/name,reviews),object_pairs_hook=unique)
    for name in ('launcher-terminal.json','watchdog-terminal.json'):
        obj=read(name)
        if obj.get('execution_id')!=eid or obj.get('status')!='completed' or type(obj.get('return_code')) is not int or obj['return_code']!=0:
            raise ValueError('supervisor termination unproven')
    process=read('harbor-process.json'); pid=process.get('pid')
    if process.get('execution_id')!=eid or type(pid) is not int or pid<=1:
        raise ValueError('native process identity invalid')
    try: os.killpg(pid,0)
    except ProcessLookupError: pass
    except PermissionError: raise ValueError('native process state unknown') from None
    else: raise ValueError('native process group active')
    steps=trial.get('step_results')
    if not isinstance(steps,list) or [x.get('step_name') for x in steps] not in (['worker'],['worker','review']):
        raise ValueError('native cleanup stages unavailable')
    for step in steps:
        name=step['step_name']; cleanup=read(name+'-planar-cleanup.json'); terminal=read(name+'-planar-terminal.json')
        if cleanup!={'execution_id':eid,'paths':['/tmp/codex-secrets','/installed-agent/codex-home'],'status':'passed'}:
            raise ValueError('guest cleanup unproven')
        if terminal!={'execution_id':eid,'status':'shell_completed','return_code':0} or type(terminal['return_code']) is not int:
            raise ValueError('agent terminal unproven')
    projects=check_projects(evidence,trial_name,trial)
    census=retained_b_census.check(launch.ROOT,eid,evidence)
    if type(census.get('matching_commands')) is not int or census['matching_commands']!=0 or type(census.get('unresolved')) is not int or census['unresolved']!=0:
        raise ValueError('command census unresolved')
    return {**census,'observed_at':datetime.now(timezone.utc).isoformat(), 'execution_id':eid,
            'native_group':'absent','guest_cleanup':'recorded and resources absent',**projects}
