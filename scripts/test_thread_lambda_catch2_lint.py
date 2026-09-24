#!/usr/bin/env python3
"""Regression tests for `thread-lambda-catch2-lint.py` (Planar plan 1069,
task 6923). Run with: `python3 -m pytest scripts/test_thread_lambda_catch2_lint.py -q`
or `python3 scripts/test_thread_lambda_catch2_lint.py`.

The load-bearing case is `test_flags_the_real_historical_bug_pattern`: it
restores the exact shape of the bug fixed at commit a59cb74f (a worker
lambda reaching a Catch2 macro through TWO helper calls,
`task_id_at` -> `scalar_int` -> `REQUIRE`) in a synthetic fixture and
requires the lint to flag it -- a lint that only looked for a macro
written literally inside the lambda would pass this fixture silently,
which is exactly the gap the real bug exploited.
"""
from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "thread_lambda_catch2_lint", ROOT / "scripts" / "thread-lambda-catch2-lint.py"
)
lint = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(lint)


HISTORICAL_BUG_SHAPE = """\
#include <vector>
#include <thread>
#include <cstdint>

namespace {

auto exec(int& conn, char const* sql) -> void {
  REQUIRE(true);
}

auto scalar_int(int& conn, char const* sql) -> std::int64_t {
  REQUIRE(true);
  return 0;
}

auto task_id_at(int& conn, int index) -> std::int64_t {
  return scalar_int(conn, "select id from tasks order by id limit 1 offset {}");
}

}  // namespace

TEST_CASE("16 threads reach a Catch2 macro through two helper calls", "[tag]") {
  int conn = 0;
  std::vector<std::jthread> workers;
  for (int i = 0; i < 16; ++i) {
    workers.emplace_back([&, i] {
      auto const task = task_id_at(conn, i);
      (void)task;
    });
  }
  REQUIRE(true);
}
"""

FIXED_SHAPE = """\
#include <vector>
#include <thread>
#include <cstdint>

namespace {

auto exec(int& conn, char const* sql) -> void {
  REQUIRE(true);
}

auto scalar_int(int& conn, char const* sql) -> std::int64_t {
  REQUIRE(true);
  return 0;
}

auto task_id_at(int& conn, int index) -> std::int64_t {
  return scalar_int(conn, "select id from tasks order by id limit 1 offset {}");
}

}  // namespace

TEST_CASE("16 threads resolve ids on the main thread first", "[tag]") {
  int conn = 0;
  std::vector<std::int64_t> task_ids;
  for (int i = 0; i < 16; ++i) {
    task_ids.push_back(task_id_at(conn, i));
  }
  std::vector<std::jthread> workers;
  for (int i = 0; i < 16; ++i) {
    workers.emplace_back([&, i] {
      auto const task = task_ids[static_cast<std::size_t>(i)];
      (void)task;
    });
  }
  REQUIRE(true);
}
"""

LITERAL_MACRO_IN_LAMBDA = """\
#include <vector>
#include <thread>

TEST_CASE("literal macro written directly inside the lambda", "[tag]") {
  std::vector<std::jthread> workers;
  for (int i = 0; i < 4; ++i) {
    workers.emplace_back([&, i] {
      REQUIRE(i >= 0);
    });
  }
}
"""


def write_probe(tmp_dir: Path, contents: str) -> Path:
    path = tmp_dir / "probe.t.cpp"
    path.write_text(contents, encoding="utf-8")
    return path


class ThreadLambdaCatch2LintTests(unittest.TestCase):
    def test_flags_the_real_historical_bug_pattern(self) -> None:
        # This is the load-bearing case: the macro is reached through TWO
        # helper calls, not written literally inside the lambda.
        with tempfile.TemporaryDirectory() as raw_tmp:
            probe = write_probe(Path(raw_tmp), HISTORICAL_BUG_SHAPE)
            violations = lint.scan_file(probe)
        self.assertTrue(violations, "expected the historical bug shape to be flagged")
        self.assertIn("task_id_at", violations[0])

    def test_fixed_shape_is_clean(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            probe = write_probe(Path(raw_tmp), FIXED_SHAPE)
            violations = lint.scan_file(probe)
        self.assertEqual(violations, [])

    def test_flags_a_literal_macro_written_directly_in_the_lambda(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            probe = write_probe(Path(raw_tmp), LITERAL_MACRO_IN_LAMBDA)
            violations = lint.scan_file(probe)
        self.assertTrue(violations)
        self.assertIn("REQUIRE", violations[0])

    def test_non_thread_emplace_back_is_ignored(self) -> None:
        source = """
        #include <vector>
        TEST_CASE("not a thread container", "[tag]") {
          std::vector<int> items;
          items.emplace_back([] { REQUIRE(true); return 0; }());
        }
        """
        with tempfile.TemporaryDirectory() as raw_tmp:
            probe = write_probe(Path(raw_tmp), source)
            violations = lint.scan_file(probe)
        self.assertEqual(violations, [])

    def test_the_real_fixed_source_file_is_clean(self) -> None:
        # Regression pin: the actual committed file (a59cb74f's fix) must
        # stay clean under this lint. If someone reverts the fix, this
        # test -- run against the real file, not a synthetic fixture --
        # fails alongside the synthetic one above.
        real_file = ROOT / "src" / "lib" / "engine" / "runtime" / "agentatomic.t.cpp"
        self.assertTrue(real_file.is_file())
        violations = lint.scan_file(real_file)
        self.assertEqual(violations, [])


if __name__ == "__main__":
    unittest.main()
