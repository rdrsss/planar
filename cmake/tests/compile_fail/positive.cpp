// A compile-time probe for the flag-declaration helpers (plan 1104, task 7200).
// It is built only by `compile_fail_check` (cmake/tests/compile_fail_driver.cmake),
// never by `all` or `planar_tests`. It lives outside src/ so clang-tidy does not
// lint the deliberately ill-formed probes.
// POSITIVE CONTROL: the same calls as the negative probes, each WITH a
// description. If this stops compiling the negative probes prove nothing, so
// the driver builds it first and fails the test when it does not build.
import std;
import cli11;
import planar.cliapp.surface;
import planar.cmd.planar.declare;
import planar.cmd.planar_agent.handlers.shared.cli;

auto probe(CLI::App& app) -> void {
  planar::cliapp::add_bool_flag(app, "--flag", "A flag.");
  planar::cmd::add_string(app, "--text", "A string.");
  planar::cmd::add_positional(app, "item", "A positional.");
  planar::cmd::agent::handlers::shared::add_json(app, "Emit JSON.");
}
