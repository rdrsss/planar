/// @file editor.cpp
/// @brief Implementation of `planar.cmd.planar.editor`.
module;

#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

module planar.cmd.planar.editor;

import std;
import planar.cmd.planar.context;

namespace planar::cmd {

namespace {

/// @brief Read an environment variable, treating EMPTY as unset.
///
/// The Zig original's `getPosixEnv` returns null for an empty value, so
/// `EDITOR=` falls through to the next candidate rather than resolving to
/// the empty command. Reproduced rather than corrected.
/// @param env The environment lookup.
/// @param key The variable name.
/// @return The value, or unset when absent or empty.
auto non_empty(const env_lookup& env, std::string_view key) -> std::optional<std::string> {
  auto value = env(key);
  if (!value.has_value() || value->empty()) {
    return std::nullopt;
  }
  return value;
}

} // namespace

auto resolve_editor(const env_lookup& env, const std::optional<std::string>& override) -> std::string {
  if (override.has_value()) {
    return *override;
  }
  for (auto const* key : {"PLANAR_EDITOR", "VISUAL", "EDITOR"}) {
    if (auto value = non_empty(env, key); value.has_value()) {
      return *value;
    }
  }
  return "vi";
}

auto resolve_program(const env_lookup& env, std::string_view program) -> std::optional<std::string> {
  if (program.empty()) {
    return std::nullopt;
  }
  auto const executable = [](const std::string& candidate) { return ::access(candidate.c_str(), X_OK) == 0; };

  if (program.contains('/')) {
    std::string direct{program};
    return executable(direct) ? std::optional{direct} : std::nullopt;
  }

  auto const path = env("PATH");
  if (!path.has_value() || path->empty()) {
    return std::nullopt;
  }
  for (auto const part : std::views::split(*path, ':')) {
    std::string_view const dir{part.begin(), part.end()};
    if (dir.empty()) {
      continue;
    }
    auto candidate = std::format("{}/{}", dir, program);
    if (executable(candidate)) {
      return candidate;
    }
  }
  return std::nullopt;
}

auto spawn_inherit(const env_lookup& env, std::span<const std::string> argv) -> std::optional<int> {
  if (argv.empty()) {
    return std::nullopt;
  }
  auto const resolved = resolve_program(env, argv[0]);
  if (!resolved.has_value()) {
    return std::nullopt;
  }

  std::vector<char*> raw;
  raw.reserve(argv.size() + 1);
  raw.push_back(const_cast<char*>(resolved->c_str()));
  for (auto const& arg : argv.subspan(1)) {
    raw.push_back(const_cast<char*>(arg.c_str()));
  }
  raw.push_back(nullptr);

  // stdout is flushed before the fork: the child INHERITS this process's
  // descriptors, so anything still sitting in this process's buffers would
  // otherwise be duplicated into the child and written twice.
  std::cout.flush();
  std::cerr.flush();

  pid_t const pid = ::fork();
  if (pid < 0) {
    return std::nullopt;
  }
  if (pid == 0) {
    // Child. `execv` is async-signal-safe; nothing else happens here.
    // stdin/stdout/stderr are left alone, which is the whole point — a
    // full-screen editor needs the real terminal.
    ::execv(raw[0], raw.data());
    ::_exit(127);
  }

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return std::nullopt;
    }
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  // Killed by a signal. The Zig original maps every non-`.exited`
  // termination to 1, and `editflow` reads this as "abort without writing".
  return 1;
}

auto invoke(const env_lookup& env, std::string_view initial_content, const invoke_opts& opts)
    -> std::expected<invoke_result, editor_error> {
  auto const command = resolve_editor(env, opts.editor_override);
  if (command.empty()) {
    return std::unexpected(editor_error::no_editor);
  }

  // The temp directory follows `$TMPDIR`, else `/tmp` — the Zig original's
  // `createTempFile`. Under test `$TMPDIR` is the fixture root, so nothing
  // escapes the scratch arena.
  auto const tmp_dir = non_empty(env, "TMPDIR").value_or("/tmp");

  std::random_device                           entropy;
  std::uniform_int_distribution<std::uint32_t> spread;
  auto const path = std::format("{}/planar-edit-{:08x}{}", tmp_dir, spread(entropy), opts.file_extension);

  {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
      return std::unexpected(editor_error::tempfile_failed);
    }
    file.write(initial_content.data(), static_cast<std::streamsize>(initial_content.size()));
    if (!file) {
      return std::unexpected(editor_error::tempfile_failed);
    }
  }

  std::vector<std::string> const argv{command, path};
  auto const                     status = spawn_inherit(env, argv);
  if (!status.has_value()) {
    std::error_code discard;
    std::filesystem::remove(path, discard);
    return std::unexpected(editor_error::tempfile_failed);
  }

  // The file is read back REGARDLESS of the exit code, matching the Zig
  // original: the caller inspects `exit_code` and decides.
  std::error_code size_ec;
  auto const      size = std::filesystem::file_size(path, size_ec);
  if (!size_ec && size > k_max_edit_file_bytes) {
    return std::unexpected(editor_error::file_too_large);
  }

  std::ifstream back(path, std::ios::binary);
  if (!back) {
    return std::unexpected(editor_error::read_failed);
  }
  std::ostringstream buffer;
  buffer << back.rdbuf();

  return invoke_result{.content = buffer.str(), .path = path, .exit_code = *status};
}

} // namespace planar::cmd
