"""Reconstruct only the validated durable A boundary in an empty workspace."""
import os
from pathlib import Path
import subprocess
import time

from source_packet import validate


def restore(packet, candidate, workspace):
    from capture import load
    from install_fixture import install
    binding = validate(packet, candidate)
    packet, workspace = Path(packet), Path(workspace)
    pre = load(packet/'source/pre')
    if pre.get('repository/RESULT.txt') != b'2\n' or pre.get('repository/STATE.txt') != b'correct\n':
        raise ValueError('source product mismatch')
    if pre.get('host/git-status.txt') != b'?? RESULT.txt\n?? STATE.txt\n': raise ValueError('source status mismatch')
    install(workspace)
    for name, data in sorted(pre.items()):
        if name.startswith('home/'):
            path = workspace/name
            if path.is_symlink(): raise ValueError('unsafe restore path')
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        elif name in ('repository/RESULT.txt','repository/STATE.txt'):
            path = workspace/name
            fd = os.open(path, os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, 'wb') as f: f.write(data)
    repo = workspace/'repository'
    env = {'PATH':os.defpath,'HOME':'/nonexistent','GIT_CONFIG_NOSYSTEM':'1',
           'GIT_CONFIG_GLOBAL':os.devnull,'GIT_TERMINAL_PROMPT':'0'}
    def git(*args):
        return subprocess.run(['git','-c','core.hooksPath=/dev/null','-C',str(repo),*args],
                              env=env,capture_output=True,check=True,timeout=30).stdout
    if git('rev-parse','HEAD') != pre['host/git-head.txt'] or git('ls-files','--stage') != pre['host/git-stage.txt'] or git('status','--porcelain=v1','--untracked-files=all') != pre['host/git-status.txt']:
        raise ValueError('rebuilt git boundary mismatch')
    if (repo/'README.md').read_bytes() != pre['repository/README.md']:
        raise ValueError('base content mismatch')
    for name, data in pre.items():
        if name.startswith('home/') or name in ('repository/RESULT.txt','repository/STATE.txt'):
            if (workspace/name).read_bytes() != data: raise ValueError('durable restore mismatch')
    # Host integration must capture and validate its own new receipt and quiescence.
    return {'source_packet_sha256':binding['source_packet_sha256'], 'restored_wall_ns':time.time_ns(),
            'restored_monotonic_ns':time.monotonic_ns(), 'source_assessment':binding['assessment']['status']}
