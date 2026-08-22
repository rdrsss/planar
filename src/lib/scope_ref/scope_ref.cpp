/// @file scope_ref.cpp
/// @brief Implementation of `planar.scope_ref` (see scope_ref.cppm).

module;

module planar.scope_ref;

import std;
import planar.db;

namespace planar::scope_ref {

auto normalize_assoc(std::string_view scope) -> std::string_view {
  constexpr std::string_view prefix = "assoc:";
  if (scope.starts_with(prefix)) {
    return scope.substr(prefix.size());
  }
  return scope;
}

auto resolve(db::connection& conn, std::string_view slug) -> std::expected<slug_ref, error> {
  if (slug == "global") {
    return slug_ref{.kind = scope_kind::global, .id = std::nullopt};
  }

  constexpr std::string_view repo_prefix = "repo:";
  if (slug.starts_with(repo_prefix)) {
    const auto bare_repo = slug.substr(repo_prefix.size());
    if (bare_repo.empty()) {
      return std::unexpected(error::slug_not_found);
    }
    auto stmt = conn.prepare("select id from projects where slug = ?");
    if (!stmt) {
      return std::unexpected(error::query_failed);
    }
    if (auto bound = stmt->bind_text(1, bare_repo); !bound) {
      return std::unexpected(error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(error::query_failed);
    }
    if (*step != db::step_result::row) {
      return std::unexpected(error::slug_not_found);
    }
    return slug_ref{.kind = scope_kind::repo, .id = stmt->column_int64(0)};
  }

  const auto bare = normalize_assoc(slug);
  if (bare.empty()) {
    return std::unexpected(error::slug_not_found);
  }
  auto stmt = conn.prepare("select id from associations where slug = ?");
  if (!stmt) {
    return std::unexpected(error::query_failed);
  }
  if (auto bound = stmt->bind_text(1, bare); !bound) {
    return std::unexpected(error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(error::slug_not_found);
  }
  return slug_ref{.kind = scope_kind::association, .id = stmt->column_int64(0)};
}

} // namespace planar::scope_ref
