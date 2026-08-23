/// @file store.cpp
/// @brief Implementation of `planar.engine.closure.store` (plan 996, task
/// 6095). See store.cppm for scope, the ordering derivation, and the cut list.

module planar.engine.closure.store;

import std;
import planar.db;

namespace planar::engine::closure::store {

namespace {

/// @brief Append `s` to `out` as a quoted JSON string — same escaping as
/// engine/runs/render.cpp's copy (zig's `output.writeJsonString`).
auto append_json_string(std::string& out, std::string_view s) -> void {
  out.push_back('"');
  for (const char raw : s) {
    const auto c = static_cast<unsigned char>(raw);
    switch (c) {
    case '\\':
      out.append("\\\\");
      break;
    case '"':
      out.append("\\\"");
      break;
    case 0x08:
      out.append("\\b");
      break;
    case 0x0C:
      out.append("\\f");
      break;
    case '\n':
      out.append("\\n");
      break;
    case '\r':
      out.append("\\r");
      break;
    case '\t':
      out.append("\\t");
      break;
    default:
      if (c <= 0x1F) {
        out.append(std::format("\\u{:04x}", static_cast<unsigned>(c)));
      } else {
        out.push_back(raw);
      }
      break;
    }
  }
  out.push_back('"');
}

} // namespace

auto show(db::connection& conn, std::int64_t task_id) -> std::expected<std::vector<row>, closure_error> {
  // The role ordering is an explicit CASE, not an alphabetical sort. It
  // happens to agree with alphabetical order for the schema's three legal
  // values, so this is transcribed rather than simplified -- an `order by
  // role` would be indistinguishable today and wrong the moment a fourth
  // role lands.
  auto stmt = conn.prepare("select id, task_id, repo_id, path, symbol, role, token_weight, "
                           "extractor_version, created_at from closures where task_id = ? "
                           "order by case role when 'modify' then 0 when 'reference' then 1 else 2 end, "
                           "path, symbol");
  if (!stmt) {
    return std::unexpected(closure_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, task_id); !bound) {
    return std::unexpected(closure_error::query_failed);
  }

  std::vector<row> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(closure_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    out.push_back(row{
        .id                = stmt->column_int64(0),
        .task_id           = stmt->column_int64(1),
        .repo_id           = stmt->column_int64(2),
        .path              = stmt->column_text(3),
        .symbol            = stmt->column_text(4),
        .role              = stmt->column_text(5),
        .token_weight      = stmt->column_int64(6),
        .extractor_version = stmt->column_text(7),
        .created_at        = stmt->column_text(8),
    });
  }
  return out;
}

auto render_show_json(std::int64_t task_id, std::span<const row> rows) -> std::string {
  std::string out = std::format("{{\"task_id\":{},\"rows\":[", task_id);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (i > 0) {
      out.push_back(',');
    }
    const auto& r = rows[i];
    // `task_id` is deliberately absent from each row object -- it is on the
    // envelope only, even though the column is read.
    out.append(std::format("{{\"id\":{},\"repo_id\":{},\"path\":", r.id, r.repo_id));
    append_json_string(out, r.path);
    out.append(",\"symbol\":");
    append_json_string(out, r.symbol);
    out.append(",\"role\":");
    append_json_string(out, r.role);
    out.append(std::format(",\"token_weight\":{},\"extractor_version\":", r.token_weight));
    append_json_string(out, r.extractor_version);
    out.append(",\"created_at\":");
    append_json_string(out, r.created_at);
    out.push_back('}');
  }
  out.append("]}");
  return out;
}

auto render_show_text(std::int64_t task_id, std::span<const row> rows) -> std::string {
  std::string out = std::format("closure for task {} ({} rows):\n", task_id, rows.size());
  if (rows.empty()) {
    // Em dash (U+2014), not a hyphen -- captured as the three bytes
    // e2 80 94 in the oracle's output.
    out.append(std::format("  (none — run `planar closure compute {}` first)\n", task_id));
    return out;
  }
  for (const auto& r : rows) {
    // Two spaces before `w=`, not one.
    out.append(std::format("  [{}] {}::{}  w={}\n", r.role, r.path, r.symbol, r.token_weight));
  }
  return out;
}

auto render_invalid_task_id(std::string_view argument) -> std::string {
  return std::format("task id must be an integer, got '{}'", argument);
}

} // namespace planar::engine::closure::store
