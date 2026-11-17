"""Explicit clean-work account disposition; never changes a behavioral outcome."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import launch
import clean_work_evidence as evidence_checks
import clean_work_live
from clean_work_release_schema import parse
from retained_b_early_evidence import read_safe, safe_path
from terminal_failure_schema import unique

canonical=evidence_checks.canonical
digest=evidence_checks.digest


def resolver_identity():
    root=launch.ROOT
    def git(*args): return subprocess.run(['git',*args],cwd=root,capture_output=True,text=True,check=True,timeout=15).stdout.strip()
    commit=git('rev-parse','HEAD')
    if len(commit)!=40 or git('status','--porcelain','--untracked-files=no'):
        raise ValueError('resolver checkout is not a clean reviewed commit')
    if git('ls-files','--others','--exclude-standard','--','evals/planar/adapters','evals/planar/cases/clean-work-review','evals/planar/experiments/codex_run.py'):
        raise ValueError('resolver inputs contain untracked files')
    for name in ('clean_work_release.py','clean_work_release_schema.py','clean_work_evidence.py','clean_work_live.py','clean_work_resources.py','clean_work_outcomes.py'):
        git('ls-files','--error-unmatch','evals/planar/adapters/'+name)
    return {'commit':commit,'implementation_sha256':launch.source_digest(launch.CASES['clean-work-review'])}


def validate_published(start, marker_path, marker):
    """Recheck reviewed authority and current absence under caller's lock."""
    try:
        import codex_run
        start=Path(start); marker_path=Path(marker_path); records=start.parent
        if codex_run._ACTIVE_ADMISSION.get()!=records.resolve(): return False
        fields={'schema','type','execution_id','review_path','review_sha256','census_path','census_sha256','review_binding'}
        if not isinstance(marker,dict) or set(marker)!=fields or marker['schema']!='planar-clean-work-account-marker-v1' or marker['type']!='clean-work-account-v1': return False
        if marker_path!=records/(marker['execution_id']+'.resolved.json'): return False
        reviews=launch.reviews_root().resolve(strict=True)
        marker_raw=read_safe(marker_path,reviews)
        if marker_raw!=canonical(marker)+b'\n': return False
        review=safe_path(marker['review_path'],reviews)
        review_raw=read_safe(review,reviews); doc=parse(review_raw)
        if digest(review_raw)!=marker['review_sha256'] or doc['execution_id']!=marker['execution_id'] or canonical(marker['review_binding'])!=canonical(doc): return False
        census_path=Path(marker['census_path'])
        if census_path.parent!=records.resolve() or census_path.name!=marker['execution_id']+'.clean-work-census-'+marker['census_sha256']+'.json': return False
        census_raw=read_safe(census_path,reviews); census=json.loads(census_raw,object_pairs_hook=unique)
        if census_raw!=canonical(census)+b'\n' or digest(canonical(census))!=marker['census_sha256'] or census.get('execution_id')!=marker['execution_id'] or census.get('native_group')!='absent' or type(census.get('matching_commands')) is not int or census['matching_commands']!=0 or type(census.get('unresolved')) is not int or census['unresolved']!=0: return False
        evidence=launch.jobs_root()/doc['execution_id']; verified=evidence_checks.verify(doc,evidence,start)
        clean_work_live.check(evidence,verified['trial_name'],verified['trial'])
        return True
    except (OSError,ValueError,TypeError,KeyError,AttributeError,subprocess.SubprocessError): return False


def publish(evidence, review):
    """Publish only the already independently reviewed account disposition."""
    sys.path.insert(0,str(launch.EVALS/'planar/experiments'))
    try:
        import codex_run
        reviews=launch.reviews_root().resolve(strict=True); evidence=Path(evidence); review=Path(review)
        review=safe_path(review,reviews)
        if review.is_relative_to(launch.jobs_root().resolve(strict=True)):
            raise ValueError('review inside job evidence')
        raw=read_safe(review,reviews); doc=parse(raw)
        if evidence.name!=doc['execution_id']: raise ValueError('execution mismatch')
        with codex_run.admission_scope() as records:
            start=records/(doc['execution_id']+'.start.json'); resolved=records/(doc['execution_id']+'.resolved.json'); closed=records/(doc['execution_id']+'.closed.json')
            if closed.exists() or closed.is_symlink(): raise ValueError('conflicting closed marker')
            verified=evidence_checks.verify(doc,evidence,start)
            if resolver_identity()!={'commit':doc['resolver_commit'],'implementation_sha256':doc['resolver_implementation_sha256']}:
                raise ValueError('resolver identity mismatch')
            census=clean_work_live.check(evidence,verified['trial_name'],verified['trial'])
            data=canonical(census)+b'\n'; census_hash=digest(canonical(census)); census_path=records/(doc['execution_id']+'.clean-work-census-'+census_hash+'.json')
            marker={'schema':'planar-clean-work-account-marker-v1','type':'clean-work-account-v1','execution_id':doc['execution_id'],'review_path':str(review.resolve(strict=True)),'review_sha256':digest(raw),'census_path':str(census_path),'census_sha256':census_hash,'review_binding':doc}
            if resolved.exists() or resolved.is_symlink():
                if resolved.is_symlink() or not resolved.is_file(): raise ValueError('conflicting resolved marker')
                existing=json.loads(read_safe(resolved,reviews),object_pairs_hook=unique)
                if not isinstance(existing,dict) or set(existing)!=set(marker) or any(canonical(existing[k])!=canonical(marker[k]) for k in marker if k not in ('census_path','census_sha256')) or not validate_published(start,resolved,existing):
                    raise ValueError('conflicting resolved marker')
                return {'execution_id':doc['execution_id'],'account':'resolved','outcome':doc['terminal_class']}
            try:
                fd=os.open(census_path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
            except FileExistsError:
                if read_safe(census_path,reviews)!=data: raise ValueError('conflicting census') from None
            else:
                with os.fdopen(fd,'wb') as f: f.write(data); f.flush(); os.fsync(f.fileno())
            directory=os.open(records,os.O_RDONLY|os.O_DIRECTORY)
            try: os.fsync(directory)
            finally: os.close(directory)
            fd,tmp=tempfile.mkstemp(prefix='.clean-work-',dir=records)
            try:
                with os.fdopen(fd,'wb') as f: f.write(canonical(marker)+b'\n'); f.flush(); os.fsync(f.fileno())
                os.link(tmp,resolved,follow_symlinks=False)
                directory=os.open(records,os.O_RDONLY|os.O_DIRECTORY)
                try: os.fsync(directory)
                finally: os.close(directory)
            finally: os.unlink(tmp)
            return {'execution_id':doc['execution_id'],'account':'resolved','outcome':doc['terminal_class']}
    finally:
        sys.path.remove(str(launch.EVALS/'planar/experiments'))
