"""Case-fixed host projection and target-only durable restore.

`prepare` runs on the trusted host after source/gate validation. `install`
runs before B inside a fresh isolated workspace, with only the sanitized
projection uploaded. Neither function accepts source transcripts as target data.
"""
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess

CASE=Path(__file__).resolve().parent
POLICY=CASE/'source-policy.json'
DURABLE_POLICY=CASE/'durable-policy.json'
BASE='08bacc341a31f32c14a1695b5b2de5c44b3a3e56'


def _selected(policy):
    result={}
    for name,meta in policy['files'].items():
        prefix='source/pre/'
        if name.startswith(prefix):
            relative=name[len(prefix):]
            if relative.startswith('home/') or relative in ('repository/RESULT.txt','repository/STATE.txt'):
                result[relative]=meta
    if not result or 'repository/RESULT.txt' not in result or 'repository/STATE.txt' not in result:
        raise ValueError('durable source policy incomplete')
    return result


def _digest(data): return {'sha256':hashlib.sha256(data).hexdigest(),'size':len(data)}


def prepare(packet, candidate, destination):
    from source_packet import validate
    from capture import load
    from safe_export import write_tree
    packet,destination=Path(packet),Path(destination)
    binding=validate(packet,candidate); policy=json.loads(POLICY.read_text()); pins=_selected(policy)
    durable=json.loads(DURABLE_POLICY.read_text())
    if durable.get('source_packet_sha256') != binding['source_packet_sha256'] or durable.get('files') != pins or durable.get('base') != BASE or durable.get('stage') != policy['files']['source/pre/host/git-stage.txt']:
        raise ValueError('durable policy mismatch')
    snapshot=load(packet/'source/pre'); selected={key:snapshot[key] for key in pins if key in snapshot}
    if set(selected)!=set(pins) or any(_digest(data)!=pins[name] for name,data in selected.items()):
        raise ValueError('durable source projection mismatch')
    if destination.exists() or destination.is_symlink(): raise ValueError('durable projection destination exists')
    destination.mkdir(mode=0o700); write_tree(destination,selected)
    return {'source_packet_sha256':binding['source_packet_sha256'],'files':len(selected),'status':'prepared'}


def install(projection, workspace, expected_source_digest):
    from install_fixture import install as install_base
    projection,workspace=Path(projection),Path(workspace); policy=json.loads(DURABLE_POLICY.read_text())
    if policy.get('schema')!='retained-b-durable-v1' or policy.get('source_packet_sha256')!=expected_source_digest or policy.get('base')!=BASE:
        raise ValueError('source packet identity mismatch')
    pins=policy.get('files'); found={}
    if not isinstance(pins,dict) or not pins: raise ValueError('durable policy incomplete')
    if projection.is_symlink() or not projection.is_dir(): raise ValueError('unsafe durable projection')
    allowed_dirs={str(p) for name in pins for p in Path(name).parents if str(p)!='.'}
    for root,dirs,files in os.walk(projection,followlinks=False):
        root=Path(root)
        for name in dirs:
            path=root/name
            if path.is_symlink() or not path.is_dir() or path.relative_to(projection).as_posix() not in allowed_dirs: raise ValueError('unsafe projection directory')
        for name in files:
            path=root/name; key=path.relative_to(projection).as_posix(); info=path.lstat()
            if key not in pins or not stat.S_ISREG(info.st_mode) or info.st_nlink!=1 or info.st_size>4*1024*1024:
                raise ValueError('unsafe projection file')
            fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW)
            with os.fdopen(fd,'rb') as f: data=f.read(4*1024*1024+1)
            if _digest(data)!=pins[key]: raise ValueError('durable projection drift')
            found[key]=data
    if set(found)!=set(pins): raise ValueError('durable projection incomplete')
    install_base(workspace)
    for name,data in sorted(found.items()):
        path=workspace/name; path.parent.mkdir(parents=True,exist_ok=True)
        flags=os.O_WRONLY|os.O_CREAT|os.O_NOFOLLOW|(os.O_TRUNC if name.startswith('home/') else os.O_EXCL)
        fd=os.open(path,flags,0o600)
        with os.fdopen(fd,'wb') as f: f.write(data)
    repo=workspace/'repository'; env={'PATH':os.defpath,'HOME':'/nonexistent','GIT_CONFIG_NOSYSTEM':'1','GIT_CONFIG_GLOBAL':os.devnull,'GIT_TERMINAL_PROMPT':'0'}
    def git(*args):
        return subprocess.run(['git','-c','core.hooksPath=/dev/null','-C',str(repo),*args],env=env,capture_output=True,check=True,timeout=30).stdout
    if git('rev-parse','HEAD')!=(BASE+'\n').encode() or git('status','--porcelain=v1','--untracked-files=all')!=b'?? RESULT.txt\n?? STATE.txt\n':
        raise ValueError('restored Git state mismatch')
    stage=git('ls-files','--stage')
    if _digest(stage)!=policy['stage']:
        raise ValueError('restored Git stage mismatch')
    if any((workspace/name).read_bytes()!=data for name,data in found.items()): raise ValueError('durable restore mismatch')
    return {'source_packet_sha256':expected_source_digest,'files':len(found),'base':BASE,'status':'restored'}
