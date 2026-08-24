/// @file engine.cpp
/// @brief Implementation of `planar.cmd.planar_execute.engine`.
module;

#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

module planar.cmd.planar_execute.engine;

import std;
import planar.cmd.planar_execute.cli;
import planar.engine_execute;

namespace planar::cmd::execute {

namespace {

/// @brief The Zig original's read limit: 8 MiB. A workflow larger than
/// this is a read failure, not a truncated load.
constexpr std::uintmax_t k_max_workflow_bytes = 8U * 1024U * 1024U;

/// @brief The running executable's own path.
///
/// Two implementations because there is no portable one. macOS answers
/// through `_NSGetExecutablePath`; everything else reads `/proc/self/exe`.
/// Both can legitimately fail (a stripped procfs, a path longer than the
/// buffer the first call reported), and failure returns empty rather than
/// guessing: `cli.*` then reports "trusted binary directory is unavailable",
/// which is a diagnosable state, whereas falling back to `PATH` would
/// quietly shell a different planar.
/// @return The absolute path, or empty.
auto executable_path() -> std::string {
#if defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size); // Reports the needed size.
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  buffer.resize(std::strlen(buffer.c_str()));
  return buffer;
#else
  std::string buffer(4096, '\0');
  auto const  len = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
  if (len <= 0 || static_cast<std::size_t>(len) >= buffer.size()) {
    return {};
  }
  buffer.resize(static_cast<std::size_t>(len));
  return buffer;
#endif
}

} // namespace

auto read_workflow(const std::filesystem::path& path) -> std::optional<std::string> {
  std::error_code ec;
  // A directory opens successfully as an ifstream on some platforms and
  // then reads zero bytes, which would look like an empty workflow rather
  // than the failure the oracle reports. Checked explicitly.
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    return std::nullopt;
  }
  auto const size = std::filesystem::file_size(path, ec);
  if (ec || size > k_max_workflow_bytes) {
    return std::nullopt;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (file.bad()) {
    return std::nullopt;
  }
  return buffer.str();
}

auto executable_dir() -> std::string {
  auto const self = executable_path();
  if (self.empty()) {
    return {};
  }
  std::error_code ec;
  // weakly_canonical so a binary reached through a symlinked bin directory
  // still resolves its siblings in the real one.
  auto const resolved = std::filesystem::weakly_canonical(std::filesystem::path{self}, ec);
  auto const base     = ec ? std::filesystem::path{self} : resolved;
  return base.parent_path().string();
}

auto run_workflow(run_args const& args, std::ostream& out, std::ostream& err) -> run_outcome {
  auto const source = read_workflow(args.workflow);
  if (!source.has_value()) {
    // Oracle bytes, verbatim — the Zig original writes the message in
    // three `errWrite` calls that concatenate to exactly this.
    err << "planar-execute: cannot read workflow: " << args.workflow << '\n';
    return run_outcome::load_failed;
  }

  engine::execute::run_config const config{
      .source       = *source,
      .phase        = args.phase,
      .args_json    = args.args_json,
      .worktree     = args.worktree,
      .sandbox_root = args.sandbox_root,
      .bin_dir      = executable_dir(),
      // ctx.now / ctx.seed stay 0. The wall clock is deliberately NOT
      // wired in: with `os` absent from the sandbox these two fields are
      // the only clock and seed a workflow can see, so defaulting them to
      // zero is what makes a run reproducible by default. A caller that
      // needs a real clock injects one through `--args`.
      .now  = 0,
      .seed = 0,
  };

  using status = engine::execute::run_status;
  return engine::execute::run_workflow(config, out, err) == status::ok ? run_outcome::ok : run_outcome::engine_failed;
}

} // namespace planar::cmd::execute
