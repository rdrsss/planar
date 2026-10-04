// A compile-time probe for the flag-declaration helpers (plan 1104, task 7200).
// It is built only by `compile_fail_check` (cmake/tests/compile_fail_driver.cmake),
// never by `all` or `planar_tests`. It lives outside src/ so clang-tidy does not
// lint the deliberately ill-formed probes.
// NEGATIVE PROBE: a call to `add_int` WITHOUT a description. It must NOT
// compile; the driver fails the test if it does, which is what happens when
// a defaulted `desc = {}` parameter is added back.
import std;
import cli11;
import planar.cliapp.surface;
import planar.cmd.planar.declare;
import planar.cmd.planar_agent.handlers.shared.cli;

auto probe(CLI::App& app) -> void {
  planar::cmd::add_int(app, "--count");
}
