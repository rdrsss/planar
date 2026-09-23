/// @file dashboard.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.dashboard`.
/// See dashboard.cppm for the `--agents` shape split and the `--scope`
/// slug-namespace note.

module planar.cmd.planar.handlers.dashboard;

import std;
import planar.cliapp.args;
import planar.db;
import planar.engine.planning.plan;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentrender;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {

namespace pl = engine::planning;
namespace aa = engine::runtime::agentactivity;
namespace ar = engine::runtime::agentrender;

namespace {

/// @brief The zig error-set spelling for a `plan_error`, so the refusal
/// body reads `plan list: SlugNotFound` exactly as the oracle's
/// `exit.die(ctx, e, "plan list: {s}", .{@errorName(e)})` does.
/// @param err The engine error.
/// @return The zig `@errorName` spelling.
auto zig_error_name(pl::plan_error err) -> std::string_view {
  switch (err) {
  case pl::plan_error::not_found:
    return "NotFound";
  case pl::plan_error::slug_conflict:
    return "SlugConflict";
  case pl::plan_error::slug_not_found:
    return "SlugNotFound";
  case pl::plan_error::invalid_parent_cycle:
    return "InvalidParentCycle";
  case pl::plan_error::illegal_transition:
    return "IllegalTransition";
  case pl::plan_error::unknown_status:
    return "UnknownStatus";
  case pl::plan_error::query_failed:
    return "QueryFailed";
  case pl::plan_error::audit_write_failed:
    return "WriteFailed";
  }
  return "Unknown";
}

/// @brief Render one claim as the oracle's single-line text form.
///
/// The four locality columns each fall back to a LITERAL `?` when NULL,
/// and `dirty_at_claim` falls back to `unknown` instead — three different
/// absence spellings in one line, all oracle-captured. The sha is
/// truncated to 8 chars only when it is at least 8 long; a shorter one is
/// emitted whole rather than padded.
/// @param out The buffer to append to.
/// @param value The claim.
auto append_claim_line(std::string& out, const aa::claim& value) -> void {
  auto const branch   = value.branch.value_or("?");
  auto const sha_full = value.head_sha_at_claim.value_or("?");
  auto const sha      = sha_full.size() >= 8 ? std::string_view{sha_full}.substr(0, 8) : std::string_view{sha_full};
  auto const dirty    = value.dirty_at_claim.has_value() ? aa::to_text(*value.dirty_at_claim) : std::string_view{"unknown"};
  auto const repo     = value.repo_root.value_or("?");
  out += std::format("  {}:{}  vendor:{}  branch:{}  sha:{}  dirty:{}  repo:{}  token:{}\n", aa::to_text(value.kind),
                     value.entity_id, value.vendor, branch, sha, dirty, repo, value.claim_token);
}

/// @brief Append one claim as JSON in the dashboard's shape.
///
/// Always carries `entity_scope` and always includes `latest_action` —
/// the oracle calls `writeClaimWithActivity(w, c, scope, true, info)` with
/// both switched on unconditionally, so this is the SAME shape
/// `planar-watch ps --json` emits and not the lean claim object.
/// @param out The buffer to append to.
/// @param conn An open connection, for the two per-claim lookups.
/// @param value The claim.
auto append_claim_object(std::string& out, db::connection& conn, const aa::claim& value) -> void {
  ar::claim_view_extras extras{};
  extras.entity_scope          = aa::resolve_claim_scope(conn, value);
  extras.include_latest_action = true;
  // A lookup failure degrades to `"latest_action":null` rather than
  // failing the verb — the oracle's `catch null`.
  if (auto row = aa::latest_action_for_claim(conn, value.id); row.has_value()) {
    extras.latest_action = *row;
  }
  ar::append_claim_view(out, value, extras);
}

} // namespace

auto dashboard(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // The EMPTY `statuses` vector is load-bearing: `plan_list_filter`
  // defaults it to draft/active/paused, which is exactly the oracle's
  // explicit `.statuses = &.{ .draft, .active, .paused }`. Spelling them
  // out here would be equivalent today and would silently diverge if that
  // default ever moved, so the default is used deliberately.
  //
  // `--scope` is passed through WHOLE. Unlike `plan list --scope` this one
  // is not comma-split.
  pl::plan_list_filter filter{};
  if (auto const scope = cliapp::flag_string(args, "--scope"); scope.has_value()) {
    filter.scope = std::string{*scope};
  }

  auto plans = pl::list_plans(**conn, filter);
  if (!plans) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("plan list: {}", zig_error_name(plans.error()))));
  }

  auto const json   = cliapp::flag_bool(args, "--json");
  auto const agents = cliapp::flag_bool(args, "--agents");

  if (!agents) {
    // The DEFAULT shape carries `active_plans` and nothing else.
    std::string out;
    if (json) {
      out += R"({"active_plans":[)";
      for (std::size_t i = 0; i < plans->size(); ++i) {
        if (i > 0) {
          out += ',';
        }
        out += pl::render_json((*plans)[i]);
      }
      out += "]}\n";
    } else {
      out += std::format("active plans: {}\n", plans->size());
      for (auto const& p : *plans) {
        out += std::format("  plan:{}  [{}]  {}\n", p.id, pl::plan_status_to_text(p.status), p.title);
      }
    }
    ctx.out() << out;
    return {};
  }

  // NO LEASE PREDICATE ON THE ACTIVE ARM. The oracle's `listActive`
  // filters on `status = 'active'` alone and orders by `claimed_at desc`,
  // which is `list_active_claims_sorted(ps_sort::lease)` here — NOT
  // `list_claims(active)`, which additionally requires an unexpired
  // lease. The observable consequence is the same one `planar-watch ps`
  // has: an expired-but-not-yet-reconciled claim appears in BOTH the
  // `active` and the `stale` list. Reproduced, and pinned in the tests.
  auto active = aa::list_active_claims_sorted(**conn, aa::ps_sort::lease);
  if (!active) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "claims list: QueryFailed"));
  }
  auto stale = aa::list_stale_claims(**conn);
  if (!stale) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "stale claims list: QueryFailed"));
  }

  std::string out;
  if (json) {
    out += R"({"active_plans":[)";
    for (std::size_t i = 0; i < plans->size(); ++i) {
      if (i > 0) {
        out += ',';
      }
      out += pl::render_json((*plans)[i]);
    }
    out += R"(],"claims":{"active":[)";
    for (std::size_t i = 0; i < active->size(); ++i) {
      if (i > 0) {
        out += ',';
      }
      append_claim_object(out, **conn, (*active)[i]);
    }
    out += R"(],"stale":[)";
    for (std::size_t i = 0; i < stale->size(); ++i) {
      if (i > 0) {
        out += ',';
      }
      append_claim_object(out, **conn, (*stale)[i]);
    }
    out += R"(]},"next_available_by_plan":{)";
    // Walked in PLAN ORDER, not map order — see the header's note on task
    // 6274. A plan whose next-work query FAILS is skipped entirely (key
    // absent); a plan with no available work gets a present, empty array.
    bool first = true;
    for (auto const& p : *plans) {
      auto rows = aa::next_work(**conn, p.id);
      if (!rows) {
        continue;
      }
      if (!first) {
        out += ',';
      }
      first = false;
      out += std::format(R"("{}":[)", p.id);
      bool task_first = true;
      for (auto const& row : *rows) {
        if (row.bucket != aa::next_work_bucket::available) {
          continue;
        }
        auto task = aa::get_task(**conn, row.task_id);
        if (!task) {
          continue;
        }
        if (!task_first) {
          out += ',';
        }
        task_first = false;
        ar::append_task(out, *task);
      }
      out += ']';
    }
    out += "}}\n";
    ctx.out() << out;
    return {};
  }

  out += std::format("active plans: {}    active claims: {}    stale claims: {}\n", plans->size(), active->size(), stale->size());
  for (auto const& p : *plans) {
    out += std::format("  plan:{}  [{}]  {}\n", p.id, pl::plan_status_to_text(p.status), p.title);
  }
  // Both claim sections are SUPPRESSED WHEN EMPTY — header included. The
  // `next available by plan:` header below is not, and prints even with no
  // plans at all.
  if (!active->empty()) {
    out += "active claims:\n";
    for (auto const& c : *active) {
      append_claim_line(out, c);
    }
  }
  if (!stale->empty()) {
    out += "stale claims:\n";
    for (auto const& c : *stale) {
      append_claim_line(out, c);
    }
  }
  out += "next available by plan:\n";
  for (auto const& p : *plans) {
    auto rows = aa::next_work(**conn, p.id);
    if (!rows) {
      continue;
    }
    auto const available =
        std::ranges::count_if(*rows, [](aa::next_work_row const& r) { return r.bucket == aa::next_work_bucket::available; });
    out += std::format("  plan:{}  available:{}\n", p.id, available);
  }
  ctx.out() << out;
  return {};
}

/// @brief Declare the `dashboard` leaf.
auto declare_dashboard(CLI::App& root) -> void {
  CLI::App* dashboard = root.add_subcommand(
      "dashboard", "Roll-up of in-flight plans in the current scope.\n\n  --agents folds in the live claim state from "
                   "agent_work_claims —\n  active claims, stale claims, and the per-plan 'next available'\n  task list. Without "
                   "--agents the dashboard is a plain plan summary.\n\n  This is the operator's read surface for agent activity; "
                   "the\n  `planar agent` subcommand namespace does not exist by design.\n  See `planar-agent` for the ritual "
                   "(claim/heartbeat/complete) and\n  `planar-watch` for the live streaming view.");
  add_string(*dashboard, "--scope", "Limit to a single scope slug");
  add_bool(*dashboard, "--agents", "Fold in live claim state + next-available-work per plan");
  add_json(*dashboard);
}

} // namespace planar::cmd::handlers
