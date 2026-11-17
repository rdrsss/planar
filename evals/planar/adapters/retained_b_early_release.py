"""Explicit typed account release for the one audited aborted-before-work run."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from datetime import datetime,timezone

import launch
import retained_b_census
import retained_b_early_evidence as ev
import retained_b_release_schema

WRAPPER_AUDIT='t045-retained-b-wrapper-provenance-disposition-20261117.md'
WRAPPER_SHA='fe6f155376212c9d77d01ecd8bb539ac99b479a5169ee754ff7282ec6c559572'


def digest(data): return hashlib.sha256(data).hexdigest()
def canonical(value): return json.dumps(value,sort_keys=True,separators=(',',':')).encode()


def resolver_identity():
    root=launch.ROOT
    def git(*args):
        result=subprocess.run(['git',*args],cwd=root,capture_output=True,text=True,timeout=15,check=True)
        return result.stdout.strip()
    commit=git('rev-parse','HEAD')
    if len(commit)!=40 or git('status','--porcelain','--untracked-files=no'): raise ValueError('resolver checkout is not a clean reviewed commit')
    if git('ls-files','--others','--exclude-standard','--','evals/planar/adapters','evals/planar/cases/retained-a-fresh-b','evals/planar/experiments/codex_run.py'):
        raise ValueError('resolver inputs contain untracked files')
    for name in ('retained_b_early_release.py','retained_b_early_evidence.py','retained_b_early_trace.py','retained_b_early_snapshots.py','retained_b_history.py','retained_b_census.py','retained_b_release_schema.py'):
        git('ls-files','--error-unmatch','evals/planar/adapters/'+name)
    return {'commit':commit,'implementation_sha256':launch.source_digest(launch.CASES['retained-a-fresh-b'])}


def cleanup_check(evidence):
    if (evidence/'recovery-required').exists() or (evidence/'recovery-required').is_symlink(): raise ValueError('recovery pending')
    for name in ('launcher-terminal.json','watchdog-terminal.json'):
        p=evidence/name
        if p.is_symlink() or json.loads(p.read_text()) .get('execution_id')!=ev.EID: raise ValueError('terminal identity mismatch')
        obj=json.loads(p.read_text())
        if obj.get('status')!='completed' or obj.get('return_code')!=0: raise ValueError('terminal outcome mismatch')
    cleanup=json.loads((evidence/'planar-cleanup.json').read_text())
    if cleanup!={'execution_id':ev.EID,'paths':['/tmp/codex-secrets','/installed-agent/codex-home'],'status':'passed'}: raise ValueError('guest cleanup unavailable')
    proc=json.loads((evidence/'harbor-process.json').read_text())
    if proc.get('execution_id')!=ev.EID or proc.get('pid')!=88111: raise ValueError('native process identity mismatch')
    try: os.killpg(88111,0)
    except ProcessLookupError: pass
    except PermissionError: raise ValueError('native group state unavailable') from None
    else: raise ValueError('native group remains alive')
    registry=evidence/'projects'
    if registry.is_symlink() or not registry.is_dir(): raise ValueError('project registry missing')
    paths=list(registry.iterdir())
    if len(paths)!=2: raise ValueError('project registry ambiguous')
    projects=set()
    for path in paths:
        if path.is_symlink() or not path.is_file() or path.suffix!='.json': raise ValueError('unsafe project record')
        obj=json.loads(path.read_text()); name=obj.get('compose_project')
        if obj.get('execution_id')!=ev.EID or not isinstance(name,str) or not name.startswith(ev.EID+'-') or not all(c.islower() or c.isdigit() or c in '_-' for c in name): raise ValueError('project identity mismatch')
        projects.add(name)
    expected={ev.EID+'-retained-a-fresh-b__qsdfojr__env',ev.EID+'-retained-a-fresh-b__qsdfojr__verifier__trial'}
    if projects!=expected: raise ValueError('project identities changed')
    for name in projects:
        for command in (['container','ls','-aq'],['network','ls','-q'],['volume','ls','-q']):
            result=subprocess.run(['docker',*command,'--filter','label=com.docker.compose.project='+name],capture_output=True,text=True,timeout=15)
            if result.returncode or result.stdout.strip(): raise ValueError('resource cleanup unconfirmed')
    return {'native_group':'absent','exact_projects':2,'guest_cleanup':'recorded-and-container-removed'}


def resolve(evidence,case,candidate,packet,audit,release):
    sys.path.insert(0,str(launch.EVALS/'planar/experiments'))
    try:
        import codex_run
        reviews=(launch.project_dir()/'reviews').resolve(strict=True); scope=launch.reviews_root().resolve(strict=True)
        evidence,case,candidate,packet,release=map(Path,(evidence,case,candidate,packet,release))
        if release.resolve(strict=True).is_relative_to(launch.jobs_root().resolve()): raise ValueError('release review inside job')
        scope_path=scope/'planar-account-admission-v1'
        if scope_path.is_symlink() or (scope_path/'lock').is_symlink() or (scope_path/'records').is_symlink(): raise ValueError('unsafe admission scope')
        with codex_run.admission_scope() as records:
            observed=ev.inspect(evidence,case,candidate,packet,audit)
            identity=resolver_identity()
            release_bytes=ev.read_safe(release,reviews); document=retained_b_release_schema.parse(release_bytes)
            gate=json.loads(ev.read_safe(scope/'approved-review.json',reviews)); wrapper=ev.read_safe(reviews/WRAPPER_AUDIT,reviews)
            if digest(wrapper)!=WRAPPER_SHA: raise ValueError('wrapper provenance changed')
            binding={'type':'aborted-before-work','decision':'release-account','execution_id':ev.EID,'job_id':ev.JOB,'trial_id':ev.TRIAL,'gate_sha256':ev.GATE_SHA,'source_packet_sha256':ev.SOURCE_SHA,'case_sha256':ev.CASE_SHA,'candidate_sha256':ev.CANDIDATE_SHA,'configuration_sha256':gate['configuration_sha256'],'historical_implementation_sha256':observed['historical_implementation']['implementation_sha256'],'resolver_commit':identity['commit'],'resolver_implementation_sha256':identity['implementation_sha256'],'audit_sha256':ev.AUDIT_SHA,'wrapper_audit_sha256':digest(wrapper),'trace_sha256':observed['trace']['trace_sha256'],'verifier_status_sha256':'d638a8043ee92815310aecbeb69060bc3a5464e08ff76f12b40252cfe0ae946e','reward_sha256':'9a271f2a916b0b6ee6cecb2426f0b3206ef074578be55d9bc94f6f3fe3ab86aa'}
            if any(document.get(k)!=v for k,v in binding.items()): raise ValueError('release review binding mismatch')
            cleanup_check(evidence)
            census=retained_b_census.check(launch.ROOT,ev.EID,evidence)
            census.update({'observed_at':datetime.now(timezone.utc).isoformat(),'historical_completion':'synchronous terminal and original tool chain','historical_launcher_pid':'unavailable','historical_watchdog_pid':'unavailable','historical_tool_exit_code':'unavailable'})
            marker={'type':'aborted-before-work','execution_id':ev.EID,'job_id':ev.JOB,'trial_id':ev.TRIAL,'observed_status':'behavioral_failure','observed_reason':'final record field mismatch','observed_reward':0,**{k:binding[k] for k in ('gate_sha256','source_packet_sha256','case_sha256','candidate_sha256','configuration_sha256','historical_implementation_sha256','resolver_commit','resolver_implementation_sha256','audit_sha256','trace_sha256','verifier_status_sha256','reward_sha256')},'release_sha256':digest(release_bytes),'release_path':str(release.resolve(strict=True)),'historical_commit':observed['historical_implementation']['commit'],'wrapper_audit_sha256':WRAPPER_SHA,'census_sha256':digest(canonical(census))}
            final=records/(ev.EID+'.resolved.json'); closed=records/(ev.EID+'.closed.json')
            if closed.exists() or closed.is_symlink(): raise ValueError('account already closed')
            if final.exists() or final.is_symlink():
                if final.is_symlink() or not final.is_file(): raise ValueError('conflicting resolution')
                previous=json.loads(final.read_text(),object_pairs_hook=retained_b_release_schema._unique)
                if not isinstance(previous,dict) or set(previous)!=set(marker) or any(type(previous[k]) is not type(marker[k]) or previous[k]!=marker[k] for k in marker if k!='census_sha256'):
                    raise ValueError('conflicting resolution')
                previous_census=records/(ev.EID+'.census-'+previous['census_sha256']+'.json')
                if previous_census.is_symlink() or not previous_census.is_file() or digest(previous_census.read_bytes().rstrip(b'\n'))!=previous['census_sha256']:
                    raise ValueError('prior census missing or changed')
                return {'execution_id':ev.EID,'account':'resolved','behavior':'aborted-before-work'}
            # Keep the full census before publishing its digest in the marker.
            census_path=records/(ev.EID+'.census-'+marker['census_sha256']+'.json'); census_bytes=canonical(census)+b'\n'
            fd=os.open(census_path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
            with os.fdopen(fd,'wb') as f: f.write(census_bytes); f.flush(); os.fsync(f.fileno())
            directory=os.open(records,os.O_RDONLY|os.O_DIRECTORY)
            try: os.fsync(directory)
            finally: os.close(directory)
            data=(json.dumps(marker,sort_keys=True)+'\n').encode()
            fd,tmp=tempfile.mkstemp(prefix='.retained-early-',dir=records)
            try:
                with os.fdopen(fd,'wb') as f: f.write(data); f.flush(); os.fsync(f.fileno())
                os.link(tmp,final,follow_symlinks=False)
                directory=os.open(records,os.O_RDONLY|os.O_DIRECTORY)
                try: os.fsync(directory)
                finally: os.close(directory)
            finally: os.unlink(tmp)
            return {'execution_id':ev.EID,'account':'resolved','behavior':'aborted-before-work'}
    finally:
        sys.path.remove(str(launch.EVALS/'planar/experiments'))
