"""Bounded child-process supervision prototype; not wired to live launch."""
import os
import signal
import subprocess
import time


class ParentDied(Exception):
    pass


def supervise(command, *, deadline, grace, cleanup, cwd=None, env=None, parent_pid=None, on_start=None):
    if deadline <= 0 or grace <= 0:
        raise ValueError('positive finite deadlines required')
    process = subprocess.Popen(command, start_new_session=True, cwd=cwd, env=env)
    try:
        if on_start is not None:
            on_start(process.pid)
    except BaseException:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=grace)
        cleanup()
        raise
    old_handlers = {}
    interrupted = None
    def on_signal(signum, frame):
        raise InterruptedError(f'received signal {signum}')
    try:
        for signum in (signal.SIGINT, signal.SIGTERM):
            old_handlers[signum] = signal.signal(signum, on_signal)
        try:
            until = time.monotonic() + deadline
            while process.poll() is None:
                if parent_pid is not None and os.getppid() != parent_pid:
                    raise ParentDied()
                if time.monotonic() >= until:
                    raise subprocess.TimeoutExpired(command, deadline)
                time.sleep(min(0.05, max(0, until - time.monotonic())))
            status = {'status': 'completed', 'return_code': process.returncode}
        except (subprocess.TimeoutExpired, InterruptedError, ParentDied) as error:
            interrupted = type(error).__name__
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=grace)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait(timeout=grace)
            status = {'status': 'interrupted', 'reason': interrupted}
    finally:
        for signum, previous in old_handlers.items():
            signal.signal(signum, previous)
        # Cleanup must validate exact resources; errors propagate as infra errors.
        cleanup()
    return status
