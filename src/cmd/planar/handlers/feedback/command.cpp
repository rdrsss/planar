/// @file src/cmd/planar/handlers/feedback/command.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.feedback`.
module;

module planar.cmd.planar.handlers.feedback;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.engine.planning;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.scope;
import planar.cmd.planar.declare;
import planar.cmd.planar.handlers.feedback.triage;
import planar.cmd.planar.handlers.feedback.triage_list;
import planar.cmd.planar.handlers.feedback.triage_show;
import planar.cmd.planar.handlers.feedback.triage_set;

namespace planar::cmd::handlers {

namespace pl = engine::planning;

namespace {

/// @brief The Zig error tag a given `feedback_triage_error` corresponds to.
///
/// The oracle's handlers fail with `exit.die(ctx, e, "<verb>: {s}",
/// .{@errorName(e)})`, so these strings are operator-visible. Five of the
/// seven were captured directly from the oracle in a pinned arena
/// (`NotFound`, `InvalidInput`, `MissingFeedbackPlan`,
/// `DifferentFeedbackPlan`, `AmbiguousFeedbackPlan`, `DuplicateCycle`);
/// `QueryFailed` is transcribed from the Zig error set because reaching it
/// needs a corrupt database.
/// @param err The engine error.
/// @return The corresponding Zig error name.
auto zig_error_name(pl::feedback_triage_error err) -> std::string_view {
  switch (err) {
  case pl::feedback_triage_error::not_found:
    return "NotFound";
  case pl::feedback_triage_error::invalid_input:
    return "InvalidInput";
  case pl::feedback_triage_error::missing_feedback_plan:
    return "MissingFeedbackPlan";
  case pl::feedback_triage_error::ambiguous_feedback_plan:
    return "AmbiguousFeedbackPlan";
  case pl::feedback_triage_error::different_feedback_plan:
    return "DifferentFeedbackPlan";
  case pl::feedback_triage_error::duplicate_cycle:
    return "DuplicateCycle";
  case pl::feedback_triage_error::query_failed:
    return "QueryFailed";
  }
  return "Unknown";
}

/// @brief Map an engine error onto this binary's exit-code bucket.
///
/// `invalid_input` is the ONE member with an arm in the oracle's `codeFor`,
/// so it exits 2 and every other member falls to the generic bucket at
/// exit 1. Captured, not assumed: `feedback triage set: InvalidInput`
/// (duplicate without a target) exits 2 while `feedback triage set:
/// DuplicateCycle` exits 1, from the same leaf on adjacent invocations.
/// @param err The engine error.
/// @param verb The verb name to lead the message with.
/// @return The mapped failure.
auto map_triage_error(pl::feedback_triage_error err, std::string_view verb) -> domain_error {
  const auto kind =
      err == pl::feedback_triage_error::invalid_input ? domain_error_kind::invalid_input : domain_error_kind::generic_failure;
  return error_from_body(kind, std::format("{}: {}", verb, zig_error_name(err)));
}

/// @brief The shared refusal for an unparseable `<finding>` positional.
auto bad_finding_ref() -> domain_error {
  return error_from_body(domain_error_kind::invalid_input, "finding must be task:<id> or question:<id>");
}

/// @brief Parse the `<finding>` positional, or produce the oracle's refusal.
auto finding_arg(const cliapp::parsed_args& args) -> std::expected<pl::feedback_finding_ref, domain_error> {
  const auto raw = cliapp::positional_string(args, "finding");
  if (!raw.has_value()) {
    return std::unexpected(bad_finding_ref());
  }
  const auto ref = pl::parse_finding_ref(*raw);
  if (!ref) {
    return std::unexpected(bad_finding_ref());
  }
  return *ref;
}

void emit(context& ctx, const cliapp::parsed_args& args, const pl::feedback_triage& row) {
  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_json(row) << '\n';
  } else {
    ctx.out() << pl::render_text(row);
  }
}

} // namespace

auto feedback_triage_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // The oracle opens the DB before validating anything, so even a refused
  // invocation leaves a migrated database behind.
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  pl::feedback_triage_list_filter filter{};
  filter.plan_id = cliapp::flag_int(args, "--plan");
  if (const auto raw = cliapp::flag_string(args, "--severity"); raw.has_value()) {
    const auto parsed = pl::feedback_severity_from_text(*raw);
    if (!parsed.has_value()) {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("unknown severity '{}'", *raw)));
    }
    filter.severity = *parsed;
  }
  if (const auto raw = cliapp::flag_string(args, "--disposition"); raw.has_value()) {
    const auto parsed = pl::feedback_disposition_from_text(*raw);
    if (!parsed.has_value()) {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("unknown disposition '{}'", *raw)));
    }
    filter.disposition = *parsed;
  }

  const auto rows = pl::list_feedback_triage(**conn, filter);
  if (!rows) {
    return std::unexpected(map_triage_error(rows.error(), "feedback triage list"));
  }
  // An unknown `--plan` is NOT validated: the oracle answers the empty
  // listing at exit 0 rather than refusing. Reproduced.
  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_list_json(*rows) << '\n';
  } else {
    ctx.out() << pl::render_list_text(*rows);
  }
  return {};
}

auto feedback_triage_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  const auto ref = finding_arg(args);
  if (!ref) {
    return std::unexpected(ref.error());
  }

  const auto row = pl::show_feedback_triage(**conn, *ref);
  if (!row) {
    if (row.error() == pl::feedback_triage_error::not_found) {
      // The message echoes the RAW positional, not the normalized ref:
      // `feedback triage show task:+2` reports `no triage row for task:+2`,
      // not `task:2`. Captured from the oracle.
      const auto raw = cliapp::positional_string(args, "finding");
      return std::unexpected(
          error_from_body(domain_error_kind::not_found, std::format("no triage row for {}", raw.value_or(""))));
    }
    return std::unexpected(map_triage_error(row.error(), "feedback triage show"));
  }
  emit(ctx, args, *row);
  return {};
}

auto feedback_triage_set(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // ---- argv-order validation -------------------------------------------
  // The finding ref is checked FIRST. A probe passing both a bad ref and a
  // bad severity gets the ref message from the oracle, so this order is
  // observable rather than stylistic.
  const auto ref = finding_arg(args);
  if (!ref) {
    return std::unexpected(ref.error());
  }

  const auto severity_raw = cliapp::flag_string(args, "--severity");
  const auto severity =
      severity_raw.has_value() ? pl::feedback_severity_from_text(*severity_raw) : std::optional<pl::feedback_severity>{};
  if (!severity.has_value()) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("unknown severity '{}'", severity_raw.value_or(""))));
  }
  const auto disposition_raw = cliapp::flag_string(args, "--disposition");
  const auto disposition     = disposition_raw.has_value() ? pl::feedback_disposition_from_text(*disposition_raw)
                                                           : std::optional<pl::feedback_disposition>{};
  if (!disposition.has_value()) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("unknown disposition '{}'", disposition_raw.value_or(""))));
  }
  const auto reproduction_raw = cliapp::flag_string(args, "--reproduction");
  const auto reproduction     = reproduction_raw.has_value() ? pl::feedback_reproduction_from_text(*reproduction_raw)
                                                             : std::optional<pl::feedback_reproduction>{};
  if (!reproduction.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                           std::format("unknown reproduction '{}'", reproduction_raw.value_or(""))));
  }

  std::optional<pl::feedback_finding_ref> duplicate_of;
  if (const auto raw = cliapp::flag_string(args, "--duplicate-of"); raw.has_value()) {
    const auto parsed = pl::parse_finding_ref(*raw);
    if (!parsed) {
      // A DIFFERENT message from the positional's, naming the flag.
      return std::unexpected(
          error_from_body(domain_error_kind::invalid_input, "--duplicate-of must be task:<id> or question:<id>"));
    }
    duplicate_of = *parsed;
  }

  // ---- scope ------------------------------------------------------------
  const auto scope_flag = cliapp::flag_string(args, "--scope");
  const auto scope_view =
      scope_flag.has_value() ? std::optional<std::string_view>{*scope_flag} : std::optional<std::string_view>{};
  const auto write = resolve_write_scope(ctx, scope_view, "feedback triage set");
  if (!write) {
    return std::unexpected(write.error());
  }

  const auto owned = pl::entity_scope(**conn, *ref);
  if (!owned) {
    // NOTE the verb prefix: `feedback finding`, not `feedback triage set`.
    // `set task:999` reports `feedback finding: NotFound` at exit 1.
    return std::unexpected(map_triage_error(owned.error(), "feedback finding"));
  }
  const auto owned_view = owned->has_value() ? std::optional<std::string_view>{**owned} : std::optional<std::string_view>{};
  const auto write_view =
      write->scope.has_value() ? std::optional<std::string_view>{*write->scope} : std::optional<std::string_view>{};
  if (!guard_with_membership(**conn, owned_view, write_view)) {
    // The advised scope is the ENTITY's, defaulting to `global` when the
    // finding is global — and a global finding never reaches here, because
    // the guard admits it from any write scope.
    return std::unexpected(
        error_from_body(domain_error_kind::scope_mismatch,
                        std::format("Refusing cross-scope write; pass --scope {} or cd into the right repo.",
                                    owned->has_value() ? std::string_view{**owned} : std::string_view{"global"})));
  }

  // ---- write -------------------------------------------------------------
  const pl::feedback_triage_set_args set_args{
      .severity     = *severity,
      .disposition  = *disposition,
      .reproduction = *reproduction,
      .duplicate_of = duplicate_of,
      .evidence     = cliapp::flag_string(args, "--evidence"),
  };
  const auto row = pl::set_feedback_triage(**conn, *ref, set_args);
  if (!row) {
    return std::unexpected(map_triage_error(row.error(), "feedback triage set"));
  }
  emit(ctx, args, *row);
  return {};
}

auto declare_feedback(CLI::App& root) -> void {
  CLI::App* feedback = root.add_subcommand("feedback", "Manage structured feedback.");
  feedback->require_subcommand(0);

  CLI::App* triage = feedback_cli::attach_triage(feedback);

  feedback_cli::attach_triage_list(triage);

  feedback_cli::attach_triage_show(triage);

  feedback_cli::attach_triage_set(triage);
}

} // namespace planar::cmd::handlers
