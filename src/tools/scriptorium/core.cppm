/// @file core.cppm
/// @brief The in-tree Scriptorium renderer: renders authored skill and agent sources into per-vendor
///   surfaces and checks staged and installed copies against them.
///
/// The one entry point is `run`, which the `scriptorium` binary calls with its arguments. Failures are
/// reported through its exit code and `err`; no exception crosses the module boundary.
export module planar.tools.scriptorium;
import std;
namespace planar::tools::scriptorium {
/// @brief Run one scriptorium verb (`render`, `check`, `status` or `version`).
/// @param args Command-line arguments after the program name.
/// @param out Receives the verb's report.
/// @param err Receives diagnostics.
/// @return 0 on success, 1 when `check` or `status` finds drift, 2 on a usage or input error.
export auto run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) -> int;
} // namespace planar::tools::scriptorium
