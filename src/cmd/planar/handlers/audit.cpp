/// @file audit.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.audit`.
/// See audit.cppm for the two-exit-code and null-optional conventions.

module planar.cmd.planar.handlers.audit;

import std;
import planar.cliapp.args;
import planar.db;
import planar.json_text;
import planar.engine.runtime.audit_trail;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace at = engine::runtime::audit_trail;

namespace {

/// @brief Left-align `text` in a field of `width`, never truncating.
///
/// The oracle's `{s:<12}` / `{d:<4}` pad to the width and OVERFLOW it when
/// the value is longer — a 20-character prefix prints all twenty
/// characters and pushes the rest of the line right. Reproduced: a
/// truncating implementation would silently lose entry-prefix text.
/// @param text The value.
/// @param width The minimum field width.
/// @return `text` padded on the right to at least `width`.
auto pad_right(std::string_view text, std::size_t width) -> std::string {
  std::string out{text};
  if (out.size() < width) {
    out.append(width - out.size(), ' ');
  }
  return out;
}

/// @brief Render the timeline as the oracle prints it without `--json`.
///
/// Header, then one line per entry. The header's task segment is `task:<id>`
/// when bound and the literal `(no task)` when not — note that the two arms
/// differ only in that segment, and both keep the same double-space
/// separators.
/// @param t The timeline.
/// @return The complete stdout payload including its trailing newline.
auto render_text(const at::session_timeline_result& t) -> std::string {
  std::string out;
  if (t.task_id.has_value()) {
    out = std::format("session {}  vendor: {}  task:{}  {}", t.session_id, t.vendor, *t.task_id, t.started_at);
  } else {
    out = std::format("session {}  vendor: {}  (no task)  {}", t.session_id, t.vendor, t.started_at);
  }
  out += t.ended_at.has_value() ? std::format(" → {}", *t.ended_at) : std::string{" → (active)"};
  out += "\n";
  for (auto const& e : t.entries) {
    out += std::format("  {}  [{}]  {}\n", pad_right(std::to_string(e.ordinal), 4), pad_right(e.prefix, 12), e.body);
  }
  return out;
}

/// @brief Render the timeline as the oracle's `--json` payload.
///
/// Key order is `id, vendor, started_at, task_id, ended_at, entries` — the
/// order of the anonymous struct the oracle's handler builds, which is NOT
/// the order of the engine's own result type. Unset optionals are OMITTED,
/// not emitted as `null`; see audit.cppm.
/// @param t The timeline.
/// @return The complete stdout payload including its trailing newline.
auto render_json(const at::session_timeline_result& t) -> std::string {
  std::string out = std::format("{{\"id\":{},\"vendor\":", t.session_id);
  out += json_text::json_string(t.vendor);
  out += ",\"started_at\":";
  out += json_text::json_string(t.started_at);
  if (t.task_id.has_value()) {
    out += std::format(",\"task_id\":{}", *t.task_id);
  }
  if (t.ended_at.has_value()) {
    out += ",\"ended_at\":";
    out += json_text::json_string(*t.ended_at);
  }
  out += ",\"entries\":[";
  for (std::size_t i = 0; i < t.entries.size(); ++i) {
    auto const& e = t.entries[i];
    if (i > 0) {
      out += ",";
    }
    out += std::format("{{\"ordinal\":{},\"prefix\":", e.ordinal);
    out += json_text::json_string(e.prefix);
    out += ",\"body\":";
    out += json_text::json_string(e.body);
    out += "}";
  }
  out += "]}\n";
  return out;
}

} // namespace

auto audit_session(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // Exit 2 for a non-integer id. `entity_id_arg` already produces the
  // oracle's exact wording (`<label> id must be an integer, got '<raw>'`)
  // at `invalid_input`.
  auto const id = entity_id_arg(args, "session-id", "session");
  if (!id) {
    return std::unexpected(id.error());
  }

  auto timeline = at::session_timeline(**conn, *id);
  if (!timeline) {
    switch (timeline.error()) {
    case at::audit_error::not_found:
      // Exit 1, not 2 — a well-formed id that names nothing is NotFound.
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("session {} not found", *id)));
    case at::audit_error::query_failed:
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "audit session: QueryFailed"));
    }
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "audit session: QueryFailed"));
  }

  ctx.out() << (cliapp::flag_bool(args, "--json") ? render_json(*timeline) : render_text(*timeline));
  return {};
}

} // namespace planar::cmd::handlers
