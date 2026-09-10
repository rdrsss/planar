/// @file drafting.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.drafting`.
module planar.cmd.planar.handlers.drafting;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.editflow;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace {

/// @brief The positional's declared name for a family, e.g. `question-id`.
/// @param kind The entity kind.
/// @return The positional name.
auto positional_for(entity_kind kind) -> std::string {
  return std::format("{}-id", entity_kind_name(kind));
}

/// @brief Parse the id the way `view` and `edit` do: integer check only.
///
/// NO positive check. `question view 0` reaches the resolver and reports
/// what the resolver finds, where `question diff 0` refuses at exit 2. The
/// asymmetry is the oracle's; see this module's interface header.
/// @param args The parsed arguments.
/// @param kind The entity kind.
/// @return The id, or the refusal.
auto lenient_id(const cliapp::parsed_args& args, entity_kind kind) -> std::expected<std::int64_t, domain_error> {
  return entity_id_arg(args, positional_for(kind), entity_kind_name(kind));
}

/// @brief Parse the id the way `diff` and `review` do: integer AND positive.
/// @param args The parsed arguments.
/// @param kind The entity kind.
/// @return The id, or the refusal.
auto strict_id(const cliapp::parsed_args& args, entity_kind kind) -> std::expected<std::int64_t, domain_error> {
  auto const id = entity_id_arg(args, positional_for(kind), entity_kind_name(kind));
  if (!id) {
    return id;
  }
  if (*id <= 0) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                           std::format("{} id must be a positive integer, got {}", entity_kind_name(kind), *id)));
  }
  return *id;
}

/// @brief The shared `view` body.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @param kind The entity kind.
/// @return Success, or the failure to report.
auto view_verb(context& ctx, const cliapp::parsed_args& args, entity_kind kind) -> handler_result {
  auto const id = lenient_id(args, kind);
  if (!id) {
    return std::unexpected(id.error());
  }
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  return view(ctx, **conn, kind, *id);
}

/// @brief The shared `edit` body.
///
/// `--no-pull` and `--json` are DECLARED on this verb and read by neither
/// the oracle nor this port. See the interface header.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @param kind The entity kind.
/// @return Success, or the failure to report.
auto edit_verb(context& ctx, const cliapp::parsed_args& args, entity_kind kind) -> handler_result {
  auto const id = lenient_id(args, kind);
  if (!id) {
    return std::unexpected(id.error());
  }
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  return edit(ctx, **conn, kind, *id, edit_opts{});
}

/// @brief The shared `diff` body.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @param kind The entity kind.
/// @return Success, or the failure to report.
auto diff_verb(context& ctx, const cliapp::parsed_args& args, entity_kind kind) -> handler_result {
  auto const id = strict_id(args, kind);
  if (!id) {
    return std::unexpected(id.error());
  }
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  return diff(ctx, **conn, kind, *id);
}

/// @brief The shared `review` body.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @param kind The entity kind.
/// @return Success, or the failure to report.
auto review_verb(context& ctx, const cliapp::parsed_args& args, entity_kind kind) -> handler_result {
  auto const id = strict_id(args, kind);
  if (!id) {
    return std::unexpected(id.error());
  }
  bool const approve         = cliapp::flag_bool(args, "--approve");
  bool const request_changes = cliapp::flag_bool(args, "--request-changes");
  // Checked BEFORE the database is opened, matching the oracle's ordering:
  // `review 1 --approve --request-changes` refuses at exit 2 having touched
  // no SQLite handle.
  if (approve && request_changes) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, "--approve and --request-changes are mutually exclusive"));
  }

  std::optional<review_verdict> verdict;
  if (approve) {
    verdict = review_verdict::approve;
  } else if (request_changes) {
    verdict = review_verdict::request_changes;
  }

  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  return review(ctx, **conn, kind, *id, verdict, cliapp::flag_bool(args, "--json"));
}

} // namespace

// `plan` and `task` reuse the four shared bodies UNCHANGED. What made them
// a task of their own rather than eight lines appended at 6205 is that the
// bodies call into `editflow` arms the other four families never reach —
// `walk_to_anchor` from a child plan, `README.md` for an anchor, and
// `task_workbench_dir`'s repo-scope/`touches`/`cross` chain — each of which
// was run against the oracle before these entry points were added. See this
// module's interface header for the one place their observable behaviour
// diverges from the other four.
auto plan_edit(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return edit_verb(ctx, args, entity_kind::plan);
}
auto plan_view(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return view_verb(ctx, args, entity_kind::plan);
}
auto plan_diff(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return diff_verb(ctx, args, entity_kind::plan);
}
auto plan_review(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return review_verb(ctx, args, entity_kind::plan);
}

auto task_edit(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return edit_verb(ctx, args, entity_kind::task);
}
auto task_view(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return view_verb(ctx, args, entity_kind::task);
}
auto task_diff(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return diff_verb(ctx, args, entity_kind::task);
}
auto task_review(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return review_verb(ctx, args, entity_kind::task);
}

auto question_edit(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return edit_verb(ctx, args, entity_kind::question);
}
auto question_view(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return view_verb(ctx, args, entity_kind::question);
}
auto question_diff(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return diff_verb(ctx, args, entity_kind::question);
}
auto question_review(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return review_verb(ctx, args, entity_kind::question);
}

auto decision_edit(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return edit_verb(ctx, args, entity_kind::decision);
}
auto decision_view(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return view_verb(ctx, args, entity_kind::decision);
}
auto decision_diff(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return diff_verb(ctx, args, entity_kind::decision);
}
auto decision_review(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return review_verb(ctx, args, entity_kind::decision);
}

auto scenario_edit(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return edit_verb(ctx, args, entity_kind::scenario);
}
auto scenario_view(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return view_verb(ctx, args, entity_kind::scenario);
}
auto scenario_diff(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return diff_verb(ctx, args, entity_kind::scenario);
}
auto scenario_review(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return review_verb(ctx, args, entity_kind::scenario);
}

auto artifact_edit(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return edit_verb(ctx, args, entity_kind::artifact);
}
auto artifact_view(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return view_verb(ctx, args, entity_kind::artifact);
}
auto artifact_diff(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return diff_verb(ctx, args, entity_kind::artifact);
}
auto artifact_review(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return review_verb(ctx, args, entity_kind::artifact);
}

} // namespace planar::cmd::handlers
