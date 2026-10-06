"""Scratch-database proof that an expired observation never replaces its job."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import queue_observation as queue


class LiveTicketTests(unittest.TestCase):
    def setUp(self) -> None:
        queue._capabilities.clear()
        self.binary = Path(__file__).resolve().parents[2] / "build/debug/bin/planar-agent"
        self.assertTrue(self.binary.is_file(), f"build the debug binary first: {self.binary}")

    def test_wait_rejects_invalid_inputs_before_observation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            env = dict(os.environ)
            env["HOME"] = str(root)
            env["PLANAR_DB"] = str(root / "planar.db")
            planar = self.binary.with_name("planar")
            subprocess.run([str(planar), "init", "--skip-project", "--allow-no-repo"],
                           cwd=root, env=env, capture_output=True, text=True, check=True)
            invalid = [
                ["0"], ["-1"], ["x"], ["9223372036854775808"],
                ["1", "--timeout", "0s"], ["1", "--timeout", "-1s"],
                ["1", "--timeout", "1.5s"], ["1", "--timeout", "1"],
                ["1", "--timeout", "999999999999999999999h"],
                ["1", "--timeout", "25h"],
            ]
            for arguments in invalid:
                with self.subTest(arguments=arguments):
                    result = subprocess.run([str(self.binary), "queue", "wait", *arguments, "--json"],
                                            cwd=root, env=env, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 2)
                    self.assertEqual(json.loads(result.stdout)["wait_reason"], "error")
            for duration in ("1ms", "24h"):
                with self.subTest(duration=duration):
                    result = subprocess.run([str(self.binary), "queue", "wait", "999999", "--timeout", duration,
                                             "--json"], cwd=root, env=env, capture_output=True, text=True)
                    envelope = json.loads(result.stdout)
                    self.assertEqual(envelope["timeout_ms"], 1 if duration == "1ms" else 86_400_000)
                    if duration == "1ms":
                        self.assertIn((result.returncode, envelope["wait_reason"]),
                                      {(1, "history_unavailable"), (124, "timed_out")})
                    else:
                        self.assertEqual((result.returncode, envelope["wait_reason"]),
                                         (1, "history_unavailable"))

    def test_native_and_older_queue_resume_original_live_ticket(self) -> None:
        for compatibility in (False, True):
            with self.subTest(compatibility=compatibility), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                env = dict(os.environ)
                env["HOME"] = str(root)
                env["PLANAR_DB"] = str(root / "planar.db")
                planar = self.binary.with_name("planar")
                subprocess.run([str(planar), "init", "--skip-project", "--allow-no-repo"],
                               cwd=root, env=env, capture_output=True, text=True, check=True)
                agent = str(self.binary)
                if compatibility:
                    wrapper = root / "older-agent"
                    wrapper.write_text(
                        "#!/usr/bin/env python3\n"
                        "import json,os,subprocess,sys\n"
                        f"binary={str(self.binary)!r}\n"
                        "if sys.argv[1:] == ['schema','--compact']:\n"
                        "    result=subprocess.run([binary,*sys.argv[1:]],check=True,capture_output=True,text=True)\n"
                        "    value=json.loads(result.stdout)\n"
                        "    value['commands']=[c for c in value['commands'] if c.get('command')!='planar-agent queue wait']\n"
                        "    print(json.dumps(value))\n"
                        "else:\n"
                        "    os.execv(binary,[binary,*sys.argv[1:]])\n",
                        encoding="utf-8",
                    )
                    wrapper.chmod(0o755)
                    agent = str(wrapper)
                marker = root / "command-completed"
                command = [sys.executable, "-c",
                           "import pathlib,time,sys; time.sleep(3); pathlib.Path(sys.argv[1]).write_text('done')",
                           str(marker)]
                seq, log_path = queue.submit(command, agent=agent, cwd=root, env=env)
                self.assertGreater(seq, 0)
                self.assertFalse(marker.exists())

                concurrent = None
                if not compatibility:
                    concurrent = subprocess.Popen(
                        [str(self.binary), "queue", "wait", str(seq), "--timeout", "1s", "--json"],
                        cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                    )

                first = queue.observe(seq, agent=agent, cwd=root, env=env, timeout_seconds=0.8)
                self.assertEqual(first.mode, "compatibility" if compatibility else "native")
                self.assertEqual(first.wait_reason, "timed_out")
                self.assertEqual(first.seq, seq)
                self.assertFalse(marker.exists(), "short observation stopped before the job finished")
                if concurrent is not None:
                    output, error = concurrent.communicate(timeout=3)
                    self.assertEqual((concurrent.returncode, error), (124, ""))
                    self.assertEqual(json.loads(output)["wait_reason"], "timed_out")
                status = subprocess.run([str(self.binary), "queue", "status", str(seq), "--json"],
                                        cwd=root, env=env, capture_output=True, text=True, check=True)
                self.assertIn(json.loads(status.stdout)["state"], {"waiting", "running", "terminating"})

                second = queue.observe(seq, agent=agent, cwd=root, env=env, timeout_seconds=12)
                self.assertEqual(second.wait_reason, "completed")
                self.assertEqual(second.result_exit_code, 0)
                self.assertEqual(second.seq, seq)
                self.assertEqual(second.observed_seq, seq)
                self.assertEqual(second.status["state"], "ended")
                self.assertEqual(second.status["outcome"], "exited")
                self.assertEqual(second.status["exit_code"], 0)
                self.assertEqual(Path(second.status["log_path"]), log_path)
                self.assertEqual(marker.read_text(encoding="utf-8"), "done")


if __name__ == "__main__":
    unittest.main()
