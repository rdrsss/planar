/// @file editor.cpp
/// @brief Implementation of `planar.cmd.planar.editor`.
module;

#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

module planar.cmd.planar.editor;

import std;
import planar.process;
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

// Both of the following moved to `planar.process` (layer 1) at task 6272,
// unchanged in behavior, so `workflow run` could reach the same runner
// without importing a module named for `$EDITOR` — and so the layer-2
// consumers still waiting on a spawn can reach it at all. These remain
// exported here because the editor/pager vocabulary is what `editflow` and
// `handlers/config.cpp` are written against; they are pure delegation.

auto resolve_program(const env_lookup& env, std::string_view program) -> std::optional<std::string> {
  return planar::process::resolve_program(env, program);
}

auto spawn_inherit(const env_lookup& env, std::span<const std::string> argv) -> std::optional<int> {
  return planar::process::run_inherited(env, argv);
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
