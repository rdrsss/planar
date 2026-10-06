"""Spec-derived detached ticket and compatibility observer checks."""

from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import queue_observation as queue


def result(argv: list[str], code: int, value: dict[str, object]) -> object:
    from subprocess import CompletedProcess
    return CompletedProcess(argv, code, json.dumps(value), "")


class QueueObservationTests(unittest.TestCase):
    def setUp(self) -> None:
        queue._capabilities.clear()
        self.cwd = Path.cwd()
        self.env = dict(os.environ)

    def test_native_completed_child_125_is_not_observer_error(self) -> None:
        status = {"seq": 7, "state": "ended", "outcome": "exited", "exit_code": 125}
        envelope = {"seq": 7, "observed_seq": 7, "wait_reason": "completed",
                    "result_exit_code": 125, "status": status, "error": None}
        with mock.patch.object(queue, "discover", return_value="native"), mock.patch.object(
            queue, "_helper", return_value=result([], 125, envelope)
        ) as helper:
            observed = queue.observe(7, agent="planar-agent", cwd=self.cwd, env=self.env)
        self.assertEqual((observed.wait_reason, observed.result_exit_code), ("completed", 125))
        self.assertEqual(observed.status, status)
        self.assertEqual(helper.call_count, 1)

    def test_older_queue_uses_json_and_stops_on_first_status_error(self) -> None:
        error = {"error": {"tag": "queue_schema_incompatible", "message": "incompatible"}}
        with mock.patch.object(queue, "discover", return_value="compatibility"), mock.patch.object(
            queue, "_helper", return_value=result([], 125, error)
        ) as helper:
            observed = queue.observe(8, agent="planar-agent", cwd=self.cwd, env=self.env)
        self.assertEqual(observed.wait_reason, "error")
        self.assertEqual(helper.call_count, 1)
        self.assertEqual(helper.call_args.args[0][-1], "--json")

    def test_abandoned_without_successor_and_empty_log_are_not_success(self) -> None:
        abandoned = {"seq": 9, "state": "ended", "outcome": "abandoned", "log_path": None}
        with mock.patch.object(queue, "discover", return_value="compatibility"), mock.patch.object(
            queue, "_helper", return_value=result([], 0, abandoned)
        ):
            observed = queue.observe(9, agent="planar-agent", cwd=self.cwd,
                                     env=self.env, timeout_seconds=0.65)
        self.assertEqual(observed.wait_reason, "timed_out")
        self.assertEqual(observed.status, abandoned)

    def test_missing_history_is_uncertainty(self) -> None:
        missing = {"error": {"tag": "not_found", "message": "missing"}}
        with mock.patch.object(queue, "discover", return_value="compatibility"), mock.patch.object(
            queue, "_helper", return_value=result([], 1, missing)
        ):
            observed = queue.observe(13, agent="planar-agent", cwd=self.cwd, env=self.env)
        self.assertEqual(observed.wait_reason, "history_unavailable")

    def test_capability_detection_distinguishes_absent_and_older_queue(self) -> None:
        def fake(args: list[str], *_args: object) -> object:
            if "schema" in args:
                return result(args, 0, {"commands": [{"command": "planar-agent queue run"}]})
            from subprocess import CompletedProcess
            return CompletedProcess(args, 0, "rule", "")
        with mock.patch.object(queue, "_helper", side_effect=fake) as helper:
            self.assertEqual(queue.discover("older-agent", self.cwd, self.env, time.monotonic() + 5), "compatibility")
            self.assertEqual(queue.discover("older-agent", self.cwd, self.env, time.monotonic() + 5), "compatibility")
        self.assertEqual(helper.call_count, 2)
        queue._capabilities.clear()
        with mock.patch.object(queue, "_helper", return_value=result([], 0, {"commands": []})):
            self.assertEqual(queue.discover("absent-agent", self.cwd, self.env, time.monotonic() + 5), "absent")

    def test_hung_status_helper_is_reaped_without_signalling_job(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            child_file = root / "pid"
            script = root / "hang.py"
            script.write_text(
                "import os,time,sys\nopen(sys.argv[1],'w').write(str(os.getpid()))\ntime.sleep(60)\n",
                encoding="utf-8",
            )
            with self.assertRaises(TimeoutError):
                queue._helper([sys.executable, str(script), str(child_file)], root,
                              self.env, time.monotonic() + 0.75)
            self.assertTrue(child_file.exists())
            pid = int(child_file.read_text())
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)

    def test_sigint_and_sigterm_reap_only_active_status_helper(self) -> None:
        if signal.getsignal(signal.SIGINT) == signal.SIG_IGN or signal.getsignal(signal.SIGTERM) == signal.SIG_IGN:
            self.skipTest("inherited ignored signal cannot be intercepted")
        for signum in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signum=signum), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                pid_file = root / "helper.pid"
                helper = root / "agent.py"
                helper.write_text(
                    "#!/usr/bin/env python3\nimport os,time\n"
                    f"open({str(pid_file)!r},'w').write(str(os.getpid()))\n"
                    "time.sleep(60)\n", encoding="utf-8",
                )
                helper.chmod(0o755)
                before = signal.getsignal(signum)

                def send_when_started() -> None:
                    for _ in range(100):
                        if pid_file.exists():
                            os.kill(os.getpid(), signum)
                            return
                        time.sleep(0.01)

                timer = threading.Thread(target=send_when_started, daemon=True)
                with mock.patch.object(queue, "discover", return_value="compatibility"):
                    timer.start()
                    observed = queue.observe(21, agent=str(helper), cwd=root,
                                             env=self.env, timeout_seconds=3)
                timer.join(timeout=2)
                self.assertEqual(observed.wait_reason, "interrupted")
                self.assertEqual(observed.result_exit_code, 128 + signum)
                self.assertIs(signal.getsignal(signum), before)
                self.assertTrue(pid_file.exists())
                with self.assertRaises(ProcessLookupError):
                    os.kill(int(pid_file.read_text()), 0)

    def test_repeated_real_status_errors_leave_no_descriptor_growth(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            helper = root / "agent.py"
            helper.write_text(
                "#!/usr/bin/env python3\nimport json,sys\n"
                "print(json.dumps({'error':{'tag':'queue_schema_incompatible','message':'refused'}}))\n"
                "sys.exit(125)\n", encoding="utf-8",
            )
            helper.chmod(0o755)
            fd_dir = Path("/dev/fd")
            before = len(list(fd_dir.iterdir())) if fd_dir.is_dir() else None
            with mock.patch.object(queue, "discover", return_value="compatibility"):
                for _ in range(5):
                    observed = queue.observe(22, agent=str(helper), cwd=root,
                                             env=self.env, timeout_seconds=2)
                    self.assertEqual(observed.wait_reason, "error")
            if before is not None:
                self.assertLessEqual(len(list(fd_dir.iterdir())), before + 1)

    def test_interrupted_helper_does_not_stop_detached_job(self) -> None:
        if signal.getsignal(signal.SIGINT) == signal.SIG_IGN:
            self.skipTest("inherited ignored SIGINT cannot be intercepted")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            helper_pid = root / "helper.pid"
            job_pid = root / "job.pid"
            helper = root / "agent.py"
            helper.write_text(
                "#!/usr/bin/env python3\nimport os,subprocess,sys,time\n"
                "job=subprocess.Popen([sys.executable,'-c','import time; time.sleep(2)'],"
                "start_new_session=True,stdin=subprocess.DEVNULL,"
                "stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)\n"
                f"open({str(job_pid)!r},'w').write(str(job.pid))\n"
                f"open({str(helper_pid)!r},'w').write(str(os.getpid()))\n"
                "time.sleep(60)\n", encoding="utf-8",
            )
            helper.chmod(0o755)

            def interrupt_when_started() -> None:
                for _ in range(100):
                    if helper_pid.exists() and job_pid.exists():
                        os.kill(os.getpid(), signal.SIGINT)
                        return
                    time.sleep(0.01)

            thread = threading.Thread(target=interrupt_when_started, daemon=True)
            with mock.patch.object(queue, "discover", return_value="compatibility"):
                thread.start()
                observed = queue.observe(23, agent=str(helper), cwd=root,
                                         env=self.env, timeout_seconds=3)
            thread.join(timeout=2)
            self.assertEqual(observed.wait_reason, "interrupted")
            with self.assertRaises(ProcessLookupError):
                os.kill(int(helper_pid.read_text()), 0)
            # Its separately detached child is still alive; cleanup addressed
            # only the observer's helper process group.
            os.kill(int(job_pid.read_text()), 0)

    def test_failed_helper_start_leaves_no_open_pipe(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fd_dir = Path("/dev/fd")
            before = len(list(fd_dir.iterdir())) if fd_dir.is_dir() else None
            with self.assertRaises(FileNotFoundError):
                queue._helper([str(root / "missing-helper")], root, self.env,
                              time.monotonic() + 2)
            if before is not None:
                self.assertLessEqual(len(list(fd_dir.iterdir())), before + 1)

    def test_failure_after_helper_started_reaps_it_and_restores_resources(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pid_file = root / "helper.pid"
            script = root / "helper.py"
            script.write_text(
                "import os,sys,time\n"
                "open(sys.argv[1],'w').write(str(os.getpid()))\n"
                "time.sleep(60)\n", encoding="utf-8",
            )
            original_communicate = subprocess.Popen.communicate
            processes: list[subprocess.Popen[str]] = []

            def injected_failure(process: subprocess.Popen[str], *args: object, **kwargs: object):
                if not processes:
                    processes.append(process)
                    for _ in range(100):
                        if pid_file.exists():
                            break
                        time.sleep(0.01)
                    raise OSError("injected pipe read failure after child creation")
                return original_communicate(process, *args, **kwargs)

            dispositions = (signal.getsignal(signal.SIGINT), signal.getsignal(signal.SIGTERM))
            fd_dir = Path("/dev/fd")
            fds_before = len(list(fd_dir.iterdir())) if fd_dir.is_dir() else None
            deadline = time.monotonic() + 2
            with mock.patch.object(subprocess.Popen, "communicate", injected_failure):
                with self.assertRaisesRegex(OSError, "injected pipe read failure"):
                    with queue._Signals():
                        queue._helper([sys.executable, str(script), str(pid_file)], root, self.env, deadline)
            self.assertTrue(pid_file.exists())
            self.assertLessEqual(time.monotonic(), deadline)
            self.assertEqual(processes[0].returncode, -signal.SIGTERM)
            self.assertTrue(processes[0].stdout.closed)
            self.assertTrue(processes[0].stderr.closed)
            with self.assertRaises(ProcessLookupError):
                os.kill(int(pid_file.read_text()), 0)
            self.assertEqual((signal.getsignal(signal.SIGINT), signal.getsignal(signal.SIGTERM)), dispositions)
            if fds_before is not None:
                self.assertLessEqual(len(list(fd_dir.iterdir())), fds_before + 1)


if __name__ == "__main__":
    unittest.main()
