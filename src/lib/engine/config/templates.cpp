/// @file templates.cpp
/// @brief Implementation of `planar.engine.config.templates` (see
/// templates.cppm).

module;

module planar.engine.config.templates;

import std;
import planar.engine.config.templates_embed;

namespace planar::engine::config {

namespace {

/// @brief Read `path` whole, returning `std::nullopt` on any failure
/// (missing file, permission denied, not a regular file, ...) — the
/// three-level chain treats every disk-read failure as "try the next
/// level", mirroring zig's `loadFromDisk`'s `catch return
/// error.TemplateNotFound`.
auto try_read_file(const std::filesystem::path& path) -> std::optional<std::string> {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    return std::nullopt;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  if (!in.good() && !in.eof()) {
    return std::nullopt;
  }
  return buf.str();
}

auto try_load_disk(std::string_view set_name, std::string_view system, std::string_view kind, const std::filesystem::path& path)
    -> std::optional<template_entry> {
  auto raw = try_read_file(path);
  if (!raw.has_value()) {
    return std::nullopt;
  }
  return template_entry{
      .set_name = std::string(set_name),
      .system   = std::string(system),
      .kind     = std::string(kind),
      .source_  = template_source::disk,
      .path     = path.string(),
      .raw      = std::move(*raw),
  };
}

} // namespace

auto load_template(std::string_view set_name, std::string_view system, std::string_view kind, std::string_view root)
    -> std::expected<template_entry, template_error> {
  // 1. <root>/<set>/<system>/<kind>.json — skip when set is "default"
  // (step 2 checks that exact path anyway).
  if (!root.empty() && set_name != "default") {
    const auto candidate = std::filesystem::path(root) / set_name / system / std::format("{}.json", kind);
    if (auto found = try_load_disk(set_name, system, kind, candidate)) {
      return std::move(*found);
    }
  }

  // 2. <root>/default/<system>/<kind>.json — baseline on disk.
  if (!root.empty()) {
    const auto candidate = std::filesystem::path(root) / "default" / system / std::format("{}.json", kind);
    if (auto found = try_load_disk("default", system, kind, candidate)) {
      return std::move(*found);
    }
  }

  // 3. Embedded defaults.
  for (const auto& entry : embedded_templates()) {
    if (entry.system != system || entry.kind != kind) {
      continue;
    }
    return template_entry{
        .set_name = std::string(set_name),
        .system   = std::string(system),
        .kind     = std::string(kind),
        .source_  = template_source::embedded,
        .path     = std::format("embedded:{}/{}.json", system, kind),
        .raw      = std::string(entry.body),
    };
  }

  return std::unexpected(template_error::not_found);
}

auto list_disk_entries(std::string_view root) -> std::vector<template_entry> {
  std::vector<template_entry> out;
  if (root.empty()) {
    return out;
  }

  std::error_code             root_ec;
  const std::filesystem::path root_path(root);
  if (!std::filesystem::is_directory(root_path, root_ec) || root_ec) {
    return out;
  }

  std::error_code sets_ec;
  for (const auto& set_entry : std::filesystem::directory_iterator(root_path, sets_ec)) {
    if (sets_ec || !set_entry.is_directory()) {
      continue;
    }
    const auto set_name = set_entry.path().filename().string();

    std::error_code sys_ec;
    for (const auto& sys_entry : std::filesystem::directory_iterator(set_entry.path(), sys_ec)) {
      if (sys_ec || !sys_entry.is_directory()) {
        continue;
      }
      const auto system_name = sys_entry.path().filename().string();

      std::error_code file_ec;
      for (const auto& file_entry : std::filesystem::directory_iterator(sys_entry.path(), file_ec)) {
        if (file_ec || !file_entry.is_regular_file()) {
          continue;
        }
        const auto file_name = file_entry.path().filename().string();
        if (!file_name.ends_with(".json")) {
          continue;
        }
        const auto kind_name = file_name.substr(0, file_name.size() - std::string_view{".json"}.size());
        out.push_back(template_entry{
            .set_name = set_name,
            .system   = system_name,
            .kind     = kind_name,
            .source_  = template_source::disk,
            .path     = file_entry.path().string(),
            .raw      = {},
        });
      }
    }
  }

  std::ranges::sort(out, [](const template_entry& a, const template_entry& b) {
    return std::tie(a.set_name, a.system, a.kind) < std::tie(b.set_name, b.system, b.kind);
  });
  return out;
}

auto list_embedded_entries() -> std::vector<template_entry> {
  std::vector<template_entry> out;
  out.reserve(embedded_templates().size());
  for (const auto& entry : embedded_templates()) {
    out.push_back(template_entry{
        .set_name = "default",
        .system   = std::string(entry.system),
        .kind     = std::string(entry.kind),
        .source_  = template_source::embedded,
        .path     = std::format("embedded:{}/{}.json", entry.system, entry.kind),
        .raw      = {},
    });
  }
  return out;
}

} // namespace planar::engine::config
