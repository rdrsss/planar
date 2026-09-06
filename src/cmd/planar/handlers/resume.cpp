/// @file resume.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.resume`.

module planar.cmd.planar.handlers.resume;

import std;
import cli11;
import planar.db;
import planar.cliapp.args;
import planar.engine.identity;
import planar.engine.runtime.resumecheck;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace rck = engine::runtime::resumecheck;

namespace {

using kind_t = domain_error_kind;

/// @brief Port of `handlers/resume/cmd.zig`'s `matchesReadScope`: does the
/// task row's stored `(scope_kind, scope_id)` land inside the cwd-derived
/// read set? A global task (`scope_id` NULL) only matches an explicit
/// global member of the set; it never leaks into a repo/association cwd.
/// @param kind The task row's `scope_kind` text column value.
/// @param id The task row's `scope_id`, unset when NULL.
/// @param scopes The cwd-derived read set.
/// @return True if some member of `scopes` matches.
auto matches_read_scope(std::string_view kind, std::optional<std::int64_t> id,
                        std::span<const engine::identity::read_scope> scopes) -> bool {
  for (auto const& scope : scopes) {
    switch (scope.kind) {
    case engine::identity::scope_kind::global:
      if (kind == "global" && !id.has_value()) {
        return true;
      }
      break;
    case engine::identity::scope_kind::association:
      if (kind == "association" && id.has_value() && *id == scope.id) {
        return true;
      }
      break;
    case engine::identity::scope_kind::repo:
      if (kind == "repo" && id.has_value() && *id == scope.id) {
        return true;
      }
      break;
    }
  }
  return false;
}

} // namespace

auto resume_packet(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  std::int64_t task_id = 0;
  auto const   raw     = positional_string(args, "task-id");
  if (raw.has_value() && !raw->empty()) {
    auto const parsed = cliapp::parse_int64_zig(*raw);
    if (!parsed) {
      return std::unexpected(error_from_body(kind_t::invalid_input, std::format("task id must be an integer, got '{}'", *raw)));
    }
    task_id = *parsed;
  } else {
    // No task-id: derive it from the cwd read scope plus the most-recent
    // active session, matching zig/src/cmd/planar/handlers/resume/cmd.zig's
    // no-id branch. `--scope` is not a flag on this verb (unlike the list
    // handlers `resolve_read_scope_slugs` serves), so the read set is
    // always cwd-derived — call the engine primitive directly rather than
    // going through `resolve_read_scope_slugs`, whose empty-set refusal
    // text is worded for `plan list`/`task list`, not this verb.
    auto const cwd = ctx.cwd().string();
    auto       set = engine::identity::resolve_read_scope_set(**conn, cwd, std::nullopt);
    if (!set) {
      // Only `scope_error::query_failed` is reachable here (no `--scope`
      // override means `slug_not_found` cannot occur).
      return std::unexpected(error_from_body(kind_t::generic_failure, "resolve scope for resume: QueryFailed"));
    }
    if (set->empty()) {
      return std::unexpected(
          error_from_body(kind_t::generic_failure,
                          "cwd is not inside any registered Planar scope; cd into a registered scope or pass "
                          "<task-id> explicitly"));
    }

    // Sessions are the activity clock for resume. Walk newest-first and
    // select the first still-active task whose stored scope is in the
    // cwd-derived read set.
    auto stmt = (*conn)->prepare("select s.task_id, t.scope_kind, t.scope_id "
                                 "from sessions s "
                                 "join tasks t on t.id = s.task_id "
                                 "where s.task_id is not null "
                                 "  and t.status in ('todo', 'doing', 'blocked') "
                                 "order by s.started_at desc, s.id desc");
    if (!stmt) {
      return std::unexpected(error_from_body(kind_t::generic_failure, "lookup recent session: QueryFailed"));
    }
    bool found = false;
    while (!found) {
      auto const stepped = stmt->step();
      if (!stepped) {
        return std::unexpected(error_from_body(kind_t::generic_failure, "lookup step: QueryFailed"));
      }
      if (*stepped == db::step_result::done) {
        break;
      }
      auto const kind_text = stmt->column_text(1);
      auto const scope_id  = stmt->is_null(2) ? std::optional<std::int64_t>{} : std::optional{stmt->column_int64(2)};
      if (matches_read_scope(kind_text, scope_id, *set)) {
        task_id = stmt->column_int64(0);
        found   = true;
      }
    }
    if (!found) {
      return std::unexpected(
          error_from_body(kind_t::not_found, "no active task in cwd-derived scope; pass <task-id> explicitly"));
    }
  }

  // The 8-section packet itself is NOT ported (see this module's header);
  // both the with-id and the derived-from-cwd task id land here.
  static_cast<void>(task_id);
  return std::unexpected(error_from_body(kind_t::not_implemented, "not implemented yet"));
}

auto resume_validate(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const raw     = positional_string(args, "task-id").value_or("");
  auto const task_id = cliapp::parse_int64_zig(raw);
  if (!task_id) {
    return std::unexpected(error_from_body(kind_t::invalid_input, std::format("task id must be an integer, got '{}'", raw)));
  }

  auto const checked = rck::validate(**conn, *task_id);
  if (!checked) {
    if (checked.error() == rck::resume_error::not_found) {
      // No payload at all on this path — stdout stays EMPTY, which is what
      // distinguishes "absent task" from "present but not resumable".
      return std::unexpected(error_from_body(kind_t::not_found, std::format("task {} not found", *task_id)));
    }
    return std::unexpected(error_from_body(kind_t::generic_failure, "resume validate: QueryFailed"));
  }

  // The payload is written FIRST and unconditionally. A non-resumable task
  // still emits its failure list and only then exits 1 — a caller that
  // reads stdout only on exit 0 would lose exactly the diagnosis it needs.
  if (flag_bool(args, "--json")) {
    ctx.out() << rck::render_validate_json(*checked);
  } else {
    ctx.out() << rck::render_validate_text(*checked);
  }

  if (!checked->resumable) {
    return std::unexpected(error_from_body(kind_t::not_found, std::format("task {} is not resumable", *task_id)));
  }
  return {};
}

} // namespace planar::cmd::handlers
