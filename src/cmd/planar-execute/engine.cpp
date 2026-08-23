/// @file engine.cpp
/// @brief Implementation of `planar.cmd.planar_execute.engine`.

module planar.cmd.planar_execute.engine;

import std;

namespace planar::cmd::execute {

namespace {

/// @brief The Zig original's read limit: 8 MiB. A workflow larger than
/// this is a read failure, not a truncated load.
constexpr std::uintmax_t k_max_workflow_bytes = 8U * 1024U * 1024U;

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

auto run_workflow(const std::filesystem::path& path, std::ostream& err) -> run_outcome {
  auto const source = read_workflow(path);
  if (!source.has_value()) {
    // Oracle bytes, verbatim — the Zig original writes the message in
    // three `errWrite` calls that concatenate to exactly this.
    err << "planar-execute: cannot read workflow: " << path.string() << '\n';
    return run_outcome::load_failed;
  }

  // The Lua seam. See this module's header: deferred WITH its dependency
  // (no Lua in this tree, no engine_execute bucket), and reported rather
  // than faked, because `{}` on stdout with exit 0 is a real oracle
  // outcome and a caller could not tell it from a missing engine.
  err << "planar-execute: the Lua workflow engine is not ported yet; "
         "read "
      << source->size() << " bytes of " << path.string() << " and stopped\n";
  return run_outcome::engine_unported;
}

} // namespace planar::cmd::execute
