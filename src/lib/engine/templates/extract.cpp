/// @file extract.cpp
/// @brief Implementation of `planar.engine.templates.extract` (plan 996,
/// task 6190). See extract.cppm for the `--force` non-behaviour.

module planar.engine.templates.extract;

import std;

namespace planar::engine::templates {

auto extract_defaults(const std::filesystem::path& root, std::span<const embedded_file> embedded, bool force)
    -> std::expected<std::vector<std::string>, extract_error> {
  if (root.empty()) {
    return std::unexpected(extract_error::invalid_input);
  }

  std::error_code ec;
  auto const      default_root = root / "default";
  std::filesystem::create_directories(default_root, ec);
  if (ec) {
    return std::unexpected(extract_error::write_failed);
  }

  std::vector<std::string> created;
  for (auto const& entry : embedded) {
    auto const sys_dir = default_root / entry.system;
    std::filesystem::create_directories(sys_dir, ec);
    if (ec) {
      return std::unexpected(extract_error::write_failed);
    }

    auto const full = sys_dir / std::format("{}.json", entry.kind);
    // EXISTS, not "is a regular file": zig probes with `access`, so a
    // directory sitting where a template belongs counts as present and is
    // skipped rather than clobbered or reported. That part is unchanged, and
    // `force` deliberately does NOT override it -- overwriting a file an
    // operator edited is what the flag is for; removing a directory tree is
    // not, and a `--force` that did it silently would be a far worse defect
    // than the one task 6321 fixed.
    if (std::filesystem::exists(full, ec)) {
      if (!force || !std::filesystem::is_regular_file(full, ec)) {
        continue;
      }
      // Fall through and truncate. TASK 6212: `--force` used to be discarded
      // entirely, so `templates init --force` exited 0, reported nothing
      // overwritten, and left the operator's edited file in place -- an
      // operator using it to reset a template they had broken was told the
      // command succeeded while nothing happened.
    }

    std::ofstream out(full, std::ios::binary | std::ios::trunc);
    if (!out) {
      return std::unexpected(extract_error::write_failed);
    }
    out.write(entry.body.data(), static_cast<std::streamsize>(entry.body.size()));
    out.close();
    if (!out) {
      return std::unexpected(extract_error::write_failed);
    }
    created.push_back(full.string());
  }
  return created;
}

} // namespace planar::engine::templates
