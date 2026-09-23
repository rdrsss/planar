/// @file ledger.cpp
/// @brief Implementation of `planar.cmd.planar_watch.handlers.ledger`.

module planar.cmd.planar_watch.handlers.ledger;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.json_text;
import planar.engine.planning.plan;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentrender;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.exit;
import planar.cmd.planar_watch.handler;
import planar.cmd.planar_watch.handlers.format;

namespace planar::cmd::watch::handlers {

namespace aa = engine::runtime::agentactivity;
namespace ar = engine::runtime::agentrender;
namespace pl = engine::planning;

using json_text::append_json_string;

namespace {

/// @brief The default row cap every `--limit`-bearing verb shares.
constexpr std::int64_t k_default_limit = 100;

/// @brief Refuse the unported streaming arm. See the module header.
/// @param verb The verb name, for the message.
/// @return The refusal.
auto follow_unsupported(std::string_view verb) -> domain_error {
  return error_from_body(domain_error_kind::not_implemented, std::format("{}: --follow is not implemented in this build", verb));
}

/// @brief Map an `agent_error` onto this binary's failure envelope,
/// preserving the reference binary's `<verb>: <ErrorName>` wording.
/// @param verb The verb name.
/// @param err The engine failure.
/// @return The reportable error.
auto engine_failure(std::string_view verb, aa::agent_error err) -> domain_error {
  return error_from_body(domain_error_kind::generic_failure, std::format("{}: {}", verb, aa::error_name(err)));
}

/// @brief `--limit`, defaulted.
/// @param args The parsed arguments.
/// @return The row cap.
auto limit_or_default(const cliapp::parsed_args& args) -> std::int64_t {
  return cliapp::flag_int(args, "--limit").value_or(k_default_limit);
}

/// @brief Does `value` survive `--vendor` and `--plan`?
/// @param conn The connection, for `--plan`'s rollup lookups.
/// @param value The claim.
/// @param args The parsed arguments.
/// @return `true` when the row should be emitted.
auto claim_matches(db::connection& conn, const aa::claim& value, const cliapp::parsed_args& args) -> bool {
  if (auto const vendor = cliapp::flag_string(args, "--vendor"); vendor.has_value() && value.vendor != *vendor) {
    return false;
  }
  if (auto const plan_id = cliapp::flag_int(args, "--plan"); plan_id.has_value()) {
    return aa::claim_belongs_to_plan(conn, value.kind, value.entity_id, *plan_id);
  }
  return true;
}

/// @brief The `kind:id` decomposition `--entity` accepts.
struct entity_ref {
  std::string  kind; ///< The stored `entity_kind` token, verbatim.
  std::int64_t id{}; ///< The entity's row id.
};

/// @brief Does `value` survive `actions`' filter set?
/// @param conn The connection, for `--plan`'s rollup lookups.
/// @param value The action.
/// @param args The parsed arguments.
/// @param entity The decomposed `--entity` / `--task` filter, when set.
/// @return `true` when the row should be emitted.
auto action_matches(db::connection& conn, const aa::action& value, const cliapp::parsed_args& args,
                    const std::optional<entity_ref>& entity) -> bool {
  if (auto const vendor = cliapp::flag_string(args, "--vendor"); vendor.has_value() && value.vendor != *vendor) {
    return false;
  }
  if (auto const kind = cliapp::flag_string(args, "--kind"); kind.has_value() && aa::to_text(value.kind) != *kind) {
    return false;
  }
  if (entity.has_value()) {
    if (!value.entity.has_value() || aa::to_text(*value.entity) != entity->kind) {
      return false;
    }
    if (!value.entity_id.has_value() || *value.entity_id != entity->id) {
      return false;
    }
  }
  if (auto const plan_id = cliapp::flag_int(args, "--plan"); plan_id.has_value()) {
    if (!value.entity.has_value() || !value.entity_id.has_value()) {
      return false;
    }
    return aa::action_belongs_to_plan(conn, *value.entity, *value.entity_id, *plan_id);
  }
  return true;
}

/// @brief Open the JSON envelope every listing verb shares.
/// @param out The buffer.
/// @param key The array key (`"claims"`, `"actions"`, …).
auto open_envelope(std::string& out, std::string_view key) -> void {
  out.append("{\"generated_at\":");
  append_json_string(out, format::now_iso());
  out.append(std::format(",\"{}\":[", key));
}

/// @brief The `scope_kind` token a plan row renders as.
///
/// Local because `planar.engine.planning.plan` exports
/// `plan_status_to_text` but no counterpart for the scope kind, and the
/// three values are a closed CHECK set. Adding an export to a layer-2
/// module for one layer-3 caller's benefit is the larger change.
/// @param kind The scope kind.
/// @return The stored token.
auto plan_scope_kind_to_text(pl::plan_scope_kind kind) -> std::string_view {
  switch (kind) {
  case pl::plan_scope_kind::repo:
    return "repo";
  case pl::plan_scope_kind::association:
    return "association";
  case pl::plan_scope_kind::global:
    return "global";
  }
  return "?";
}

/// @brief Append a plan as a JSON object, in `planar plan show --json`'s
/// field order.
/// @param out The buffer.
/// @param value The plan row.
auto append_plan(std::string& out, const pl::plan& value) -> void {
  out.append(std::format("{{\"id\":{},\"scope_kind\":", value.id));
  append_json_string(out, plan_scope_kind_to_text(value.scope_kind));
  if (value.scope_id.has_value()) {
    out.append(std::format(",\"scope_id\":{}", *value.scope_id));
  } else {
    out.append(",\"scope_id\":null");
  }
  out.append(",\"title\":");
  append_json_string(out, value.title);
  out.append(",\"slug\":");
  append_json_string(out, value.slug);
  out.append(",\"summary\":");
  if (value.summary.has_value()) {
    append_json_string(out, *value.summary);
  } else {
    out.append("null");
  }
  out.append(",\"status\":");
  append_json_string(out, pl::plan_status_to_text(value.status));
  if (value.parent_plan_id.has_value()) {
    out.append(std::format(",\"parent_plan_id\":{}", *value.parent_plan_id));
  } else {
    out.append(",\"parent_plan_id\":null");
  }
  out.append(",\"created_at\":");
  append_json_string(out, value.created_at);
  out.append(",\"updated_at\":");
  append_json_string(out, value.updated_at);
  out.push_back('}');
}

} // namespace

auto claims(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  if (cliapp::flag_bool(args, "--follow")) {
    return std::unexpected(follow_unsupported("claims"));
  }
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const flag   = cliapp::flag_string(args, "--status");
  auto const filter = aa::parse_claim_status_filter(flag.has_value() ? std::optional<std::string_view>{*flag} : std::nullopt);
  auto       rows   = aa::list_claims(**conn, filter);
  if (!rows) {
    return std::unexpected(engine_failure("claims", rows.error()));
  }

  std::string out;
  bool const  json = cliapp::flag_bool(args, "--json");
  if (json) {
    open_envelope(out, "claims");
  } else {
    // The UNFILTERED count. See the module header.
    out.append(std::format("claims: {}\n", rows->size()));
  }

  bool first = true;
  for (auto const& row : *rows) {
    if (!claim_matches(**conn, row, args)) {
      continue;
    }
    auto scope = aa::resolve_claim_scope(**conn, row);
    auto sup   = aa::get_supervision(**conn, row.claim_token);
    if (!sup) {
      return std::unexpected(engine_failure("claims", sup.error()));
    }
    if (json) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      ar::append_claim_view(out, row, ar::claim_view_extras{.entity_scope = std::move(scope), .supervision = *sup});
      continue;
    }
    // An engine-supervised claim whose lease has passed is `lapsed (engine)`,
    // not `active`: reconcile deliberately leaves it for the engine's own
    // recovery (plan 1033 task 6489), so it would otherwise read as live.
    std::string status{aa::to_text(row.status)};
    if (row.status == aa::claim_status::active && sup->engine) {
      auto const live = aa::is_claim_active_unexpired(**conn, row.claim_token);
      if (live && !*live) {
        status = "lapsed (engine)";
      }
    }
    // Engine claims name their supervisor and attempt; a caller claim's line
    // is exactly what it always was (task 6493).
    std::string const supervised =
        sup->engine ? std::format("  supervisor:engine  attempt:{}", sup->attempt_id.value_or("-")) : std::string{};
    if (row.category.has_value()) {
      out.append(std::format("  {}:{}  scope:{}  status:{}  vendor:{}  category:{}{}  token:{}\n", aa::to_text(row.kind),
                             row.entity_id, scope.label(), status, row.vendor, aa::to_text(*row.category), supervised,
                             row.claim_token));
    } else {
      out.append(std::format("  {}:{}  scope:{}  status:{}  vendor:{}{}  token:{}\n", aa::to_text(row.kind), row.entity_id,
                             scope.label(), status, row.vendor, supervised, row.claim_token));
    }
  }
  if (json) {
    out.append("]}\n");
  }

  ctx.out() << out;
  return {};
}

auto actions(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  if (cliapp::flag_bool(args, "--follow")) {
    return std::unexpected(follow_unsupported("actions"));
  }
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  std::optional<entity_ref> entity;
  if (auto const raw = cliapp::flag_string(args, "--entity"); raw.has_value()) {
    auto const colon = raw->find(':');
    if (colon == std::string::npos) {
      // The reference binary reports the ERROR NAME here, not the offending
      // value — `actions` funnels every emit failure through one
      // `"actions: {errorName}"` die site. `log` below is the one that
      // spells the value out, and that asymmetry is the oracle's.
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, "actions: InvalidInput"));
    }
    auto const id = cliapp::parse_int64_zig(std::string_view{*raw}.substr(colon + 1));
    if (!id.has_value()) {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, "actions: InvalidInput"));
    }
    entity = entity_ref{.kind = raw->substr(0, colon), .id = *id};
  }
  // `--task N` is sugar for `--entity task:N`, and OVERRIDES `--entity`
  // when both appear (last assignment wins in the original).
  if (auto const task_id = cliapp::flag_int(args, "--task"); task_id.has_value()) {
    entity = entity_ref{.kind = "task", .id = *task_id};
  }

  auto rows = aa::list_actions(**conn, limit_or_default(args));
  if (!rows) {
    return std::unexpected(engine_failure("actions", rows.error()));
  }

  std::string out;
  bool const  json = cliapp::flag_bool(args, "--json");
  if (json) {
    open_envelope(out, "actions");
  } else {
    out.append(std::format("actions: {}\n", rows->size()));
  }

  bool first = true;
  for (auto const& row : *rows) {
    if (!action_matches(**conn, row, args, entity)) {
      continue;
    }
    if (json) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      ar::append_action(out, row);
      continue;
    }
    // A NULL entity renders as `-` with id `0` — the columns stay aligned
    // and the row is still identifiable by its action id.
    std::string_view const entity_kind = row.entity.has_value() ? aa::to_text(*row.entity) : std::string_view{"-"};
    out.append(std::format("  action:{}  kind:{}  vendor:{}  entity:{}:{}  started:{}\n", row.id, aa::to_text(row.kind),
                           row.vendor, entity_kind, row.entity_id.value_or(0), row.started_at));
  }
  if (json) {
    out.append("]}\n");
  }

  ctx.out() << out;
  return {};
}

auto plans(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  if (cliapp::flag_bool(args, "--follow")) {
    return std::unexpected(follow_unsupported("plans"));
  }
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // EVERY status, not just active: the operator wants to see in-flight work
  // on a paused or draft plan too. `--in-flight-only` is the narrowing.
  pl::plan_list_filter const filter{.statuses = {pl::plan_status::draft, pl::plan_status::active, pl::plan_status::paused,
                                                 pl::plan_status::done, pl::plan_status::abandoned}};
  auto                       rows = pl::list_plans(**conn, filter);
  if (!rows) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "plans: QueryFailed"));
  }

  bool const  json           = cliapp::flag_bool(args, "--json");
  bool const  in_flight_only = cliapp::flag_bool(args, "--in-flight-only");
  std::string out;
  if (json) {
    open_envelope(out, "plans");
  } else {
    out.append(std::format("plans: {}\n", rows->size()));
  }

  bool first = true;
  for (auto const& row : *rows) {
    auto stats = aa::collect_plan_activity(**conn, row.id);
    if (!stats) {
      return std::unexpected(engine_failure("plans", stats.error()));
    }
    bool const in_flight = stats->active_claims > 0 || stats->active_actions > 0;
    if (in_flight_only && !in_flight) {
      continue;
    }
    if (json) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      out.append("{\"plan\":");
      append_plan(out, row);
      out.append(std::format(",\"in_flight\":{}", in_flight ? "true" : "false"));
      out.append(std::format(",\"active_claims\":{}", stats->active_claims));
      out.append(std::format(",\"active_actions\":{}", stats->active_actions));
      out.append(",\"last_event_at\":");
      if (stats->last_event_at.has_value()) {
        append_json_string(out, *stats->last_event_at);
      } else {
        out.append("null");
      }
      out.push_back('}');
      continue;
    }
    out.append(std::format("  plan:{}  [{}]  in_flight:{}  claims:{}  actions:{}  {}\n", row.id,
                           pl::plan_status_to_text(row.status), in_flight ? "yes" : "no", stats->active_claims,
                           stats->active_actions, row.title));
  }
  if (json) {
    out.append("]}\n");
  }

  ctx.out() << out;
  return {};
}

namespace {

/// @brief One entry in `log`'s merged timeline.
///
/// A claim contributes ONE event when it is still active (`claim_acquired`)
/// and TWO when it is not — acquisition plus a terminal event named after
/// the status it reached. An action contributes one, at `started_at`.
struct timeline_event {
  std::string      at;   ///< The occurrence timestamp, sorted on.
  std::string_view kind; ///< The `"kind"` discriminator.
  /// @brief Index into the action vector, when this is an action event.
  std::optional<std::size_t> action_index;
  /// @brief Index into the claim vector, when this is a claim event.
  std::optional<std::size_t> claim_index;
};

/// @brief The terminal event name for a non-active claim status.
/// @param status The claim's status.
/// @return The `"kind"` value.
auto terminal_event_kind(aa::claim_status status) -> std::string_view {
  switch (status) {
  case aa::claim_status::released:
    return "claim_released";
  case aa::claim_status::completed:
    return "claim_completed";
  case aa::claim_status::aborted:
    return "claim_aborted";
  case aa::claim_status::stale:
    return "claim_stale";
  case aa::claim_status::active:
    break;
  }
  // Unreachable: the caller tests `status != active` first. Returning a
  // token rather than asserting keeps a corrupt row renderable.
  return "claim_stale";
}

} // namespace

auto log(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const task_id    = cliapp::flag_int(args, "--task");
  auto const plan_id    = cliapp::flag_int(args, "--plan");
  auto const entity_raw = cliapp::flag_string(args, "--entity");
  auto const session_id = cliapp::flag_int(args, "--session");
  auto const claim_tok  = cliapp::flag_string(args, "--claim");

  int const supplied = static_cast<int>(task_id.has_value()) + static_cast<int>(plan_id.has_value()) +
                       static_cast<int>(entity_raw.has_value()) + static_cast<int>(session_id.has_value()) +
                       static_cast<int>(claim_tok.has_value());
  if (supplied != 1) {
    return std::unexpected(error_from_body(
        domain_error_kind::invalid_input,
        std::format("log: exactly one of --task / --plan / --entity / --session / --claim required (got {})", supplied)));
  }

  std::optional<std::string>  entity_kind;
  std::optional<std::int64_t> entity_id;
  if (task_id.has_value()) {
    entity_kind = "task";
    entity_id   = *task_id;
  } else if (plan_id.has_value()) {
    entity_kind = "plan";
    entity_id   = *plan_id;
  } else if (entity_raw.has_value()) {
    auto const colon = entity_raw->find(':');
    if (colon == std::string::npos) {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                             std::format("log: --entity expects kind:id (got '{}')", *entity_raw)));
    }
    auto const parsed = cliapp::parse_int64_zig(std::string_view{*entity_raw}.substr(colon + 1));
    if (!parsed.has_value()) {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                             std::format("log: --entity id is not an integer ('{}')", *entity_raw)));
    }
    entity_kind = entity_raw->substr(0, colon);
    entity_id   = *parsed;
  }

  auto const limit = limit_or_default(args);

  std::expected<std::vector<aa::claim>, aa::agent_error>  claim_rows;
  std::expected<std::vector<aa::action>, aa::agent_error> action_rows;
  if (claim_tok.has_value()) {
    claim_rows  = aa::list_claims_by_token(**conn, *claim_tok);
    action_rows = aa::list_actions_by_claim_token(**conn, *claim_tok, limit);
  } else if (session_id.has_value()) {
    claim_rows  = aa::list_claims_by_session(**conn, *session_id);
    action_rows = aa::list_actions_by_session(**conn, *session_id, limit);
  } else {
    claim_rows  = aa::list_claims_by_entity(**conn, *entity_kind, *entity_id);
    action_rows = aa::list_actions_by_entity(**conn, *entity_kind, *entity_id, limit);
  }
  if (!claim_rows) {
    return std::unexpected(engine_failure("log", claim_rows.error()));
  }
  if (!action_rows) {
    return std::unexpected(engine_failure("log", action_rows.error()));
  }

  std::string out;
  if (!cliapp::flag_bool(args, "--json")) {
    // Text mode does NOT merge: claims first, then actions, each in its own
    // query order. Only the JSON arm interleaves by timestamp. That is the
    // reference binary's split and it is preserved.
    out.append(std::format("entries: {} actions + {} claim rows\n", action_rows->size(), claim_rows->size()));
    for (auto const& row : *claim_rows) {
      out.append(std::format("  claim:{}  {}  vendor:{}  status:{}\n", row.claim_token, row.claimed_at, row.vendor,
                             aa::to_text(row.status)));
    }
    for (auto const& row : *action_rows) {
      out.append(std::format("  action:{}  {}  kind:{}  vendor:{}\n", row.id, row.started_at, aa::to_text(row.kind), row.vendor));
    }
    ctx.out() << out;
    return {};
  }

  out.append("{\"entity\":{");
  if (claim_tok.has_value()) {
    out.append("\"kind\":\"claim_token\",\"id\":");
    append_json_string(out, *claim_tok);
  } else if (session_id.has_value()) {
    out.append(std::format("\"kind\":\"session\",\"id\":{}", *session_id));
  } else {
    out.append("\"kind\":");
    append_json_string(out, *entity_kind);
    out.append(std::format(",\"id\":{}", *entity_id));
  }
  out.append("},\"entries\":[");

  std::vector<timeline_event> events;
  for (std::size_t i = 0; i < action_rows->size(); ++i) {
    events.push_back(timeline_event{.at = (*action_rows)[i].started_at, .kind = "action", .action_index = i, .claim_index = {}});
  }
  for (std::size_t i = 0; i < claim_rows->size(); ++i) {
    auto const& row = (*claim_rows)[i];
    events.push_back(timeline_event{.at = row.claimed_at, .kind = "claim_acquired", .action_index = {}, .claim_index = i});
    if (row.status != aa::claim_status::active) {
      // `released_at` is the terminal instant. The fallback to the
      // heartbeat is DEFENSIVE ONLY and is currently UNREACHABLE, which is
      // worth stating rather than leaving as an implied edge case: every
      // statement in `agentactivity` that moves a claim off `active`
      // (`release_claim`, `abort_claim`, `reconcile_stale`) sets
      // `released_at` in the same UPDATE, and so does every one of their
      // Zig counterparts — checked, not assumed. A break-probe swapping
      // this fallback for `claimed_at` therefore SURVIVES: no verb can
      // produce the row that would tell them apart.
      //
      // Kept anyway, and kept matching the reference binary, because a
      // NULL here would otherwise sort the event to the front of the
      // timeline. Same call `agentactivity.t.cpp` records for
      // `worktree_id`: the branch is implemented, it simply has no state
      // to fire against today.
      events.push_back(timeline_event{.at           = row.released_at.value_or(row.last_heartbeat_at),
                                      .kind         = terminal_event_kind(row.status),
                                      .action_index = {},
                                      .claim_index  = i});
    }
  }
  // Fixed-width ISO-8601 UTC, so lexicographic IS chronological.
  // `stable_sort` because ties must keep insertion order (actions before
  // claims), which `sort` is free to break.
  std::stable_sort(events.begin(), events.end(), [](const timeline_event& a, const timeline_event& b) { return a.at < b.at; });

  bool first = true;
  for (auto const& event : events) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    out.append("{\"kind\":");
    append_json_string(out, event.kind);
    out.append(",\"at\":");
    append_json_string(out, event.at);
    if (event.action_index.has_value()) {
      out.append(",\"action\":");
      ar::append_action(out, (*action_rows)[*event.action_index]);
    }
    if (event.claim_index.has_value()) {
      out.append(",\"claim\":");
      // The LEAN claim shape — no `entity_scope`. A timeline entry is
      // already scoped by the `entity` header above it; `ps` and `claims`
      // are the surfaces that carry the scope per row.
      ar::append_claim(out, (*claim_rows)[*event.claim_index]);
    }
    out.push_back('}');
  }
  out.append("]}\n");

  ctx.out() << out;
  return {};
}

} // namespace planar::cmd::watch::handlers
