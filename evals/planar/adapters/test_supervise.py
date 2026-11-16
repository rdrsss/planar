import sys
import unittest
from unittest.mock import Mock

from supervise import supervise


class SupervisionControls(unittest.TestCase):
    def test_timeout_terminates_child_and_runs_cleanup(self):
        cleanup = Mock()
        result = supervise([sys.executable, '-c', 'import time; time.sleep(30)'], deadline=0.05, grace=2, cleanup=cleanup)
        self.assertEqual(result['status'], 'interrupted')
        self.assertEqual(result['reason'], 'TimeoutExpired')
        cleanup.assert_called_once()

    def test_cleanup_failure_propagates(self):
        cleanup = Mock(side_effect=RuntimeError('unconfirmed'))
        with self.assertRaisesRegex(RuntimeError, 'unconfirmed'):
            supervise([sys.executable, '-c', 'pass'], deadline=2, grace=2, cleanup=cleanup)

    def test_sigterm_reaps_child_and_runs_cleanup(self):
        import os
        import signal
        import subprocess
        import tempfile
        import time
        from pathlib import Path
        with tempfile.TemporaryDirectory() as tmp:
            marker = Path(tmp) / 'cleaned'
            script = ('import sys; from pathlib import Path; from supervise import supervise; '
                      'supervise([sys.executable,"-c","import time; time.sleep(30)"], '
                      'deadline=30, grace=2, cleanup=lambda:Path(sys.argv[1]).write_text("done"))')
            proc = subprocess.Popen([sys.executable, '-c', script, str(marker)], env={**os.environ, 'PYTHONPATH': str(Path(__file__).parent)})
            try:
                time.sleep(0.25)
                proc.send_signal(signal.SIGTERM)
                self.assertEqual(proc.wait(timeout=4), 0)
                self.assertEqual(marker.read_text(), 'done')
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=4)

    def test_parent_death_triggers_watchdog_cleanup(self):
        import os
        import subprocess
        import tempfile
        import time
        from pathlib import Path
        with tempfile.TemporaryDirectory() as tmp:
            marker = Path(tmp) / 'cleaned'
            guardian = ('import os,sys; from pathlib import Path; from supervise import supervise; '
                        's=supervise([sys.executable,"-c","import time; time.sleep(30)"], '
                        'deadline=10,grace=2,parent_pid=int(sys.argv[1]), '
                        'cleanup=lambda:Path(sys.argv[2]).write_text("done")); '
                        'Path(sys.argv[2]+".result").write_text(s["reason"])')
            parent = ('import os,subprocess,sys,time; '
                      'subprocess.Popen([sys.executable,"-c",sys.argv[1],str(os.getpid()),sys.argv[2]]); '
                      'time.sleep(0.3)')
            env = {**os.environ, 'PYTHONPATH':str(Path(__file__).parent)}
            subprocess.run([sys.executable, '-c', parent, guardian, str(marker)], env=env, timeout=5, check=True)
            for _ in range(80):
                if marker.exists() and Path(str(marker)+'.result').exists():
                    break
                time.sleep(0.05)
            self.assertEqual(marker.read_text(), 'done')
            self.assertEqual(Path(str(marker)+'.result').read_text(), 'ParentDied')
