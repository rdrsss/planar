/// @file fsutil.cpp
/// @brief Implementation of `planar.engine.workbench.fsutil` (plan 996, task
/// 6037). See fsutil.cppm for why the atomic write is tmp-then-rename and
/// why `delete_tree` refuses a symlink.

module planar.engine.workbench.fsutil;

import std;

namespace planar::engine::workbench::fsutil {

auto read_file(const std::filesystem::path& path) -> std::optional<std::string> {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return std::nullopt;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

auto make_path_all(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  return std::filesystem::is_directory(path, ec);
}

auto path_exists(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

auto write_file_atomic(const std::filesystem::path& path, std::string_view content) -> bool {
  if (path.has_parent_path() && !make_path_all(path.parent_path())) {
    return false;
  }
  auto const tmp = std::filesystem::path{path.string() + ".tmp"};
  {
    std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
    if (!file) {
      return false;
    }
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file) {
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  return true;
}

auto collect_markdown(const std::filesystem::path& dir) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::error_code          ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return out;
  }
  // `skip_permission_denied` mirrors the Zig walk's `opendir` failure arm,
  // which silently returns rather than aborting the whole sync.
  std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    return out;
  }
  for (auto const& entry : it) {
    std::error_code entry_ec;
    if (!entry.is_regular_file(entry_ec)) {
      continue;
    }
    if (entry.path().extension() == ".md") {
      out.push_back(entry.path().string());
    }
  }
  return out;
}

auto delete_tree(const std::filesystem::path& dir) -> bool {
  std::error_code ec;
  auto const      status = std::filesystem::symlink_status(dir, ec);
  if (ec || !std::filesystem::exists(status)) {
    return false;
  }
  // `symlink_status`, not `status`: a symlink pointing AT a directory must
  // not qualify. See fsutil.cppm.
  if (!std::filesystem::is_directory(status)) {
    return false;
  }
  std::filesystem::remove_all(dir, ec);
  return !ec;
}

auto remove_file(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  return !ec;
}

} // namespace planar::engine::workbench::fsutil
