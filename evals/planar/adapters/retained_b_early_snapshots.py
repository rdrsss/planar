"""Verify the three fixed early-stop host snapshots without changing them."""
import hashlib
import json
import os
from pathlib import Path
import stat


def manifest(root):
    root=Path(root)
    if root.is_symlink() or not root.is_dir(): raise ValueError('unsafe snapshot root')
    path=root/'snapshot-manifest.json'
    if path.is_symlink() or not path.is_file(): raise ValueError('snapshot manifest missing')
    expected=json.loads(path.read_text()); actual={}
    if not isinstance(expected,dict) or len(expected)>128: raise ValueError('invalid snapshot manifest')
    for parent,dirs,files in os.walk(root,followlinks=False):
        for name in dirs:
            p=Path(parent)/name
            if p.is_symlink(): raise ValueError('snapshot directory link')
        for name in files:
            p=Path(parent)/name; key=p.relative_to(root).as_posix()
            if key=='snapshot-manifest.json': continue
            info=p.lstat()
            if not stat.S_ISREG(info.st_mode) or info.st_nlink!=1 or info.st_size>4*1024*1024: raise ValueError('unsafe snapshot file')
            data=p.read_bytes(); actual[key]={'sha256':hashlib.sha256(data).hexdigest(),'size':len(data)}
    if actual!=expected: raise ValueError('snapshot inventory or contents differ')
    return actual


def inspect(root):
    root=Path(root); pre,post,final=(manifest(root/x) for x in ('restored-pre','post','final'))
    if set(pre)!=set(post) or post!=final: raise ValueError('final snapshot differs from post-injection')
    changed={k for k in pre if pre[k]!=post[k]}
    if changed!={'host/repository-inventory.json','repository/STATE.txt'}: raise ValueError('unexpected pre/post changes')
    injection_path=root/'injection.json'
    if injection_path.is_symlink() or not injection_path.is_file(): raise ValueError('injection receipt missing')
    receipt=json.loads(injection_path.read_text()); change=receipt.get('change',{})
    if change.get('changed')!='repository/STATE.txt' or change.get('before')!=pre['repository/STATE.txt'] or change.get('after')!=post['repository/STATE.txt']:
        raise ValueError('injection receipt mismatch')
    prior=json.loads((root/'restored-pre/host/repository-inventory.json').read_text()); after=json.loads((root/'post/host/repository-inventory.json').read_text())
    if set(prior)!=set(after): raise ValueError('repository inventory shape differs')
    for key in prior:
        if key!='files' and prior[key]!=after[key]: raise ValueError('repository metadata differs')
    if set(prior['files'])!=set(after['files']) or {k for k in prior['files'] if prior['files'][k]!=after['files'][k]}!={'STATE.txt'}:
        raise ValueError('repository inventory change differs')
    return {'entries':len(pre),'post_final_equal':True,'changed':'repository/STATE.txt'}
