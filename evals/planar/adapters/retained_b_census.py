"""Same-user, exact reviewed command census; never emits argv or environment."""
import ctypes
import errno
import os
from pathlib import Path
import subprocess
import sys


def _argv(pid):
    if sys.platform != 'darwin': raise ValueError('unsupported process census platform')
    libc=ctypes.CDLL(None,use_errno=True); mib=(ctypes.c_int*3)(1,49,pid); size=ctypes.c_size_t()
    if libc.sysctl(mib,3,None,ctypes.byref(size),None,0)!=0: raise OSError(ctypes.get_errno(),'process argv unavailable')
    if not 4<size.value<2*1024*1024: raise ValueError('invalid process argv size')
    buf=ctypes.create_string_buffer(size.value)
    if libc.sysctl(mib,3,buf,ctypes.byref(size),None,0)!=0: raise OSError(ctypes.get_errno(),'process argv unavailable')
    raw=buf.raw[:size.value]; argc=int.from_bytes(raw[:4],sys.byteorder,signed=True)
    if not 0<argc<4096: raise ValueError('invalid process argc')
    tail=raw[4:]; end=tail.find(b'\0')
    if end<0: raise ValueError('process executable missing')
    tail=tail[end+1:].lstrip(b'\0'); args=tail.split(b'\0',argc)
    if len(args)<argc or any(not x for x in args[:argc]): raise ValueError('process argv incomplete')
    return [os.fsdecode(x) for x in args[:argc]]


def check(root, execution_id, evidence, *, argv_reader=_argv, run=subprocess.run):
    root=Path(root).resolve(strict=True); evidence=Path(evidence).resolve(strict=True)
    proc=run(['ps','-axo','uid=,pid=,ppid=,comm='],capture_output=True,text=True,timeout=15)
    if proc.returncode: raise ValueError('process census failed')
    rows={}
    for line in proc.stdout.splitlines():
        parts=line.strip().split(None,3)
        if len(parts)!=4: raise ValueError('process census malformed')
        try: uid,pid,ppid=map(int,parts[:3])
        except ValueError: raise ValueError('process census malformed') from None
        rows[pid]=(uid,ppid,parts[3])
    exclude=set(); current=os.getpid()
    while current and current not in exclude:
        exclude.add(current)
        if current not in rows: raise ValueError('observer ancestry unavailable')
        current=rows[current][1]
    programs={str(root/'evals/planar/adapters'/x) for x in ('launch.py','watchdog.py','native_job.py')}
    selected=0
    for pid,(uid,_,command) in rows.items():
        if uid!=os.getuid() or pid in exclude: continue
        name=Path(command).name.lower()
        if not (name in ('sh','bash','zsh','uv','python','python3') or name.startswith('python3.')): continue
        selected+=1
        try: argv=argv_reader(pid)
        except (OSError,ValueError,UnicodeError): raise ValueError('process identity unavailable') from None
        for arg in argv:
            try: path=str(Path(arg).resolve()) if arg.endswith(('launch.py','watchdog.py','native_job.py')) else ''
            except OSError: raise ValueError('process path unavailable') from None
            if path in programs: raise ValueError('reviewed execution command remains active')
            # A shell wrapper may contain an entire command as one argument.
            if any(program in arg for program in programs) or execution_id in arg or str(evidence) in arg:
                raise ValueError('execution identity remains active or ambiguous')
    return {'same_user_candidates_checked':selected,'matching_commands':0,'unresolved':0,
            'historical_launcher_pid':'unavailable','historical_watchdog_pid':'unavailable',
            'meaning':'current exact-command absence; not global account idleness'}
