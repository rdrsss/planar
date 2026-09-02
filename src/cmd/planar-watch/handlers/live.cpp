/// @file live.cpp
/// @brief Implementation of `planar.cmd.planar_watch.handlers.live`.

module planar.cmd.planar_watch.handlers.live;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.json_text;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentrender;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.exit;
import planar.cmd.planar_watch.handler;
import planar.cmd.planar_watch.handlers.format;
import planar.cmd.planar_watch.handlers.follow;

namespace planar::cmd::watch::handlers {

namespace aa = engine::runtime::agentactivity;
namespace ar = engine::runtime::agentrender;

using json_text::append_json_string;

namespace {

/// @brief `--group-by`'s three dimensions.
enum class group_by : std::uint8_t {
  role,   ///< The claim's `role`; `unknown` when NULL.
  scope,  ///< The resolved storage-scope label of the claim's entity.
  vendor, ///< The claim's `vendor`.
};

/// @brief Parse `--group-by`.
/// @param text The flag value, or unset when absent.
/// @return Unset outer for "flag absent"; unset inner for "bad value".
auto parse_group_by(const std::optional<std::string>& text) -> std::optional<std::optional<group_by>> {
  if (!text.has_value()) {
    return std::optional<group_by>{};
  }
  if (*text == "role") {
    return std::optional<group_by>{group_by::role};
  }
  if (*text == "scope") {
    return std::optional<group_by>{group_by::scope};
  }
  if (*text == "vendor") {
    return std::optional<group_by>{group_by::vendor};
  }
  return std::nullopt;
}

/// @brief Refuse the unported streaming arm. See the module header.
/// @param verb The verb name.
/// @return The refusal.
auto follow_unsupported(std::string_view verb) -> domain_error {
  return error_from_body(domain_error_kind::not_implemented, std::format("{}: --follow is not implemented in this build", verb));
}

/// @brief Map an `agent_error` onto this binary's failure envelope.
/// @param verb The verb name.
/// @param err The engine failure.
/// @return The reportable error.
auto engine_failure(std::string_view verb, aa::agent_error err) -> domain_error {
  return error_from_body(domain_error_kind::generic_failure, std::format("{}: {}", verb, aa::error_name(err)));
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

/// @brief Everything one claim row needs beyond the row itself: where its
/// entity lives, and what it was last doing.
struct row_context {
  aa::claim_scope_info      scope;         ///< Resolved storage scope.
  std::optional<aa::action> latest_action; ///< Newest action on the claim.
};

/// @brief Gather a row's scope and latest action.
///
/// A failing latest-action lookup degrades to "no action" rather than
/// failing the whole render, matching zig's `catch null` at both call
/// sites: a viewer that refused to list a claim because one auxiliary
/// query hiccuped would be worse than one that shows `activity:""`.
/// @param conn The connection.
/// @param value The claim.
/// @return The gathered context.
auto gather(db::connection& conn, const aa::claim& value) -> row_context {
  row_context out{.scope = aa::resolve_claim_scope(conn, value), .latest_action = std::nullopt};
  if (auto found = aa::latest_action_for_claim(conn, value.id); found.has_value()) {
    out.latest_action = std::move(*found);
  }
  return out;
}

/// @brief Render one `ps` claim line, terminator included.
/// @param value The claim.
/// @param ctx_row The gathered scope and latest action.
/// @param now_ms The reference instant for the `last_hb:` column.
/// @return The line.
auto render_claim_line(const aa::claim& value, const row_context& ctx_row, std::int64_t now_ms) -> std::string {
  std::string_view const branch = value.branch.has_value() ? std::string_view{*value.branch} : std::string_view{"?"};
  std::string_view const sha_full =
      value.head_sha_at_claim.has_value() ? std::string_view{*value.head_sha_at_claim} : std::string_view{"?"};
  auto const sha = sha_full.size() >= 8 ? sha_full.substr(0, 8) : sha_full;

  auto const activity =
      format::render_activity_summary(ctx_row.latest_action.has_value() ? ctx_row.latest_action->summary : std::nullopt);
  auto const worktree  = format::render_worktree_column(value.worktree_path);
  auto const heartbeat = format::render_relative_heartbeat(now_ms, value.last_heartbeat_at);

  if (value.category.has_value()) {
    return std::format("  {}:{}  scope:{}  activity:{}  vendor:{}  branch:{}  worktree:{}  sha:{}  last_hb:{}  category:{}  "
                       "token:{}\n",
                       aa::to_text(value.kind), value.entity_id, ctx_row.scope.label(), activity, value.vendor, branch, worktree,
                       sha, heartbeat, aa::to_text(*value.category), value.claim_token);
  }
  return std::format("  {}:{}  scope:{}  activity:{}  vendor:{}  branch:{}  worktree:{}  sha:{}  last_hb:{}  token:{}\n",
                     aa::to_text(value.kind), value.entity_id, ctx_row.scope.label(), activity, value.vendor, branch, worktree,
                     sha, heartbeat, value.claim_token);
}

/// @brief Append one `ps` claim as JSON, with both optional fields.
/// @param out The buffer.
/// @param value The claim.
/// @param ctx_row The gathered scope and latest action.
auto append_ps_claim(std::string& out, const aa::claim& value, const row_context& ctx_row) -> void {
  // `include_latest_action` is unconditionally true here: `ps` emits the
  // field even when there is no action, as `"latest_action":null`.
  ar::append_claim_view(out, value,
                        ar::claim_view_extras{.entity_scope          = ctx_row.scope,
                                              .include_latest_action = true,
                                              .latest_action         = ctx_row.latest_action});
}

/// @brief One filtered claim paired with its group key and its gathered
/// context.
///
/// The context is CARRIED rather than re-derived per group pass: the
/// grouped arms visit every row once per unique key, and re-running the
/// scope resolution and latest-action lookup inside that nested loop would
/// make the query count quadratic in the number of groups.
struct grouped_row {
  std::string key;           ///< The group key.
  std::size_t claim_index{}; ///< Index into the merged claim vector.
  row_context context;       ///< The row's resolved scope and latest action.
};

} // namespace

auto ps(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  if (cliapp::flag_bool(args, "--follow") && !follow::active())
    return follow::snapshots(ctx, cliapp::flag_string(args, "--interval"), [&] { return ps(ctx, args); });

  auto const sort_flag = cliapp::flag_string(args, "--sort-by");
  auto const sort      = aa::parse_ps_sort(sort_flag.has_value() ? std::optional<std::string_view>{*sort_flag} : std::nullopt);
  if (!sort.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           "ps: --sort-by: accepted values are 'heartbeat' (default) or 'lease'"));
  }
  auto const grouping = parse_group_by(cliapp::flag_string(args, "--group-by"));
  if (!grouping.has_value()) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, "ps: --group-by: accepted values are 'role', 'scope', or 'vendor'"));
  }

  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto active = aa::list_active_claims_sorted(**conn, *sort);
  if (!active) {
    return std::unexpected(engine_failure("ps", active.error()));
  }
  std::vector<aa::claim> stale;
  if (cliapp::flag_bool(args, "--stale")) {
    auto found = aa::list_stale_claims(**conn);
    if (!found) {
      return std::unexpected(engine_failure("ps", found.error()));
    }
    stale = std::move(*found);
  }

  auto const  now  = format::now_ms();
  bool const  json = cliapp::flag_bool(args, "--json");
  std::string out;

  if (!grouping->has_value()) {
    // --- ungrouped -------------------------------------------------------
    if (json) {
      out.append("{\"generated_at\":");
      append_json_string(out, format::now_iso());
      out.append(",\"active\":[");
      bool first = true;
      for (auto const& row : *active) {
        if (!claim_matches(**conn, row, args)) {
          continue;
        }
        if (!first) {
          out.push_back(',');
        }
        first = false;
        append_ps_claim(out, row, gather(**conn, row));
      }
      out.append("],\"stale\":[");
      first = true;
      for (auto const& row : stale) {
        if (!claim_matches(**conn, row, args)) {
          continue;
        }
        if (!first) {
          out.push_back(',');
        }
        first = false;
        append_ps_claim(out, row, gather(**conn, row));
      }
      out.append("]}\n");
      ctx.out() << out;
      return {};
    }

    // `--vendor` AND `--plan` ARE IGNORED IN THIS ARM. That is not an
    // omission — it is the reference binary's behavior, and it was found by
    // diffing rather than by reading: `zig/src/cmd/planar-watch/handlers/
    // ps.zig`'s `emitText` is the ONE emitter of the four that takes no
    // `args` parameter at all, so `claimMatches` is never reached from it.
    // `ps --vendor codex` prints every active claim; `ps --vendor codex
    // --json` and `ps --vendor codex --group-by vendor` both filter
    // correctly. Verified against `zig/zig-out/bin/planar-watch` on a seeded
    // scratch database, and pinned by `handlers.t.cpp`'s
    // "ps ignores --vendor in the ungrouped text arm" case so nobody
    // "fixes" it into a silent divergence.
    //
    // Reproduced under D2. If it is ever to be corrected, that is a change
    // to the reference binary first.
    out.append(std::format("active: {}\n", active->size()));
    for (auto const& row : *active) {
      out.append(render_claim_line(row, gather(**conn, row), now));
    }
    if (!stale.empty()) {
      out.append(std::format("stale: {}\n", stale.size()));
      for (auto const& row : stale) {
        out.append(render_claim_line(row, gather(**conn, row), now));
      }
    }
    ctx.out() << out;
    return {};
  }

  // --- grouped -----------------------------------------------------------
  // Both buckets merge into ONE flat sequence; the active/stale envelope
  // does not survive `--group-by`, in either arm.
  std::vector<aa::claim> merged;
  merged.reserve(active->size() + stale.size());
  merged.insert(merged.end(), active->begin(), active->end());
  merged.insert(merged.end(), stale.begin(), stale.end());

  auto const               dimension = **grouping;
  std::vector<grouped_row> rows;
  for (std::size_t i = 0; i < merged.size(); ++i) {
    if (!claim_matches(**conn, merged[i], args)) {
      continue;
    }
    auto        gathered = gather(**conn, merged[i]);
    std::string key;
    switch (dimension) {
    case group_by::role:
      key = merged[i].role.value_or("unknown");
      break;
    case group_by::vendor:
      key = merged[i].vendor;
      break;
    case group_by::scope:
      key = std::string{gathered.scope.label()};
      break;
    }
    rows.push_back(grouped_row{.key = std::move(key), .claim_index = i, .context = std::move(gathered)});
  }

  // Unique keys in FIRST-SEEN order, not sorted: the reference binary
  // collects them by insertion and the resulting group order is therefore a
  // function of the claim ordering, which `--sort-by` controls.
  std::vector<std::string> keys;
  for (auto const& row : rows) {
    if (std::find(keys.begin(), keys.end(), row.key) == keys.end()) {
      keys.push_back(row.key);
    }
  }

  if (json) {
    out.append("{\"generated_at\":");
    append_json_string(out, format::now_iso());
    out.append(",\"groups\":{");
    bool first_group = true;
    for (auto const& key : keys) {
      if (!first_group) {
        out.push_back(',');
      }
      first_group = false;
      append_json_string(out, key);
      out.append(":[");
      bool first_row = true;
      for (auto const& row : rows) {
        if (row.key != key) {
          continue;
        }
        if (!first_row) {
          out.push_back(',');
        }
        first_row = false;
        append_ps_claim(out, merged[row.claim_index], row.context);
      }
      out.append("]");
    }
    out.append("}}\n");
    ctx.out() << out;
    return {};
  }

  if (rows.empty()) {
    // The one place the grouped text arm falls back to the ungrouped
    // header: with nothing to group, `active: 0` is what the operator sees.
    ctx.out() << "active: 0\n";
    return {};
  }
  for (auto const& key : keys) {
    out.append(std::format("[group: {}]\n", key));
    for (auto const& row : rows) {
      if (row.key != key) {
        continue;
      }
      out.append(render_claim_line(merged[row.claim_index], row.context, now));
    }
  }
  ctx.out() << out;
  return {};
}

auto tree(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  if (cliapp::flag_bool(args, "--follow") && !follow::active())
    return follow::snapshots(ctx, cliapp::flag_string(args, "--interval"), [&] { return tree(ctx, args); });
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const root_session = cliapp::flag_int(args, "--root-session");
  if (root_session.has_value()) {
    if (*root_session < 1) {
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, "tree: --root-session: must be a positive integer"));
    }
    auto const exists = aa::session_exists(**conn, *root_session);
    if (!exists) {
      return std::unexpected(engine_failure("tree", exists.error()));
    }
    if (!*exists) {
      // `not_found` and `generic_failure` both exit 1 here, so the choice is
      // about naming rather than the code — but naming it `not_found` is
      // what makes the exit table's rows honest.
      return std::unexpected(error_from_body(domain_error_kind::not_found,
                                             std::format("tree: --root-session {}: session not found", *root_session)));
    }
  }

  auto nodes = aa::walk_action_forest(**conn, root_session);
  if (!nodes) {
    return std::unexpected(engine_failure("tree", nodes.error()));
  }
  if (nodes->empty()) {
    ctx.out() << "(no action chains)\n";
    return {};
  }

  auto const now = format::now_ms();

  // `open_at_depth[d]` — does the ancestor at depth `d` still have later
  // siblings? Drives the `│` continuation guides. 64 is zig's bound; a
  // deeper forest simply stops drawing guides rather than reallocating,
  // which is also zig's behavior.
  constexpr std::size_t         k_max_depth = 64;
  std::array<bool, k_max_depth> open_at_depth{};

  std::string out;
  for (auto const& node : *nodes) {
    auto const depth = node.depth >= 0 ? static_cast<std::size_t>(node.depth) : std::size_t{0};
    if (depth < open_at_depth.size()) {
      open_at_depth[depth] = !node.is_last_sibling;
      // Deeper levels belong to a different subtree now.
      for (std::size_t d = depth + 1; d < open_at_depth.size(); ++d) {
        open_at_depth[d] = false;
      }
    }

    if (depth == 0) {
      out.append(std::format("action:{}  ", node.id));
    } else {
      for (std::size_t level = 1; level < depth; ++level) {
        out.append(level < open_at_depth.size() && open_at_depth[level] ? "\xE2\x94\x82   " : "    ");
      }
      out.append(node.is_last_sibling ? "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 " : "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 ");
    }

    bool rendered_claim = false;
    if (node.claim_id.has_value()) {
      if (auto claim_row = aa::get_claim_by_id(**conn, *node.claim_id); claim_row.has_value()) {
        auto const             gathered = gather(**conn, *claim_row);
        std::string_view const branch =
            claim_row->branch.has_value() ? std::string_view{*claim_row->branch} : std::string_view{"?"};
        auto const activity =
            format::render_activity_summary(gathered.latest_action.has_value() ? gathered.latest_action->summary : std::nullopt);
        out.append(std::format("scope:{}  vendor:{}  activity:{}  worktree:{}  branch:{}  last_hb:{}\n", gathered.scope.label(),
                               claim_row->vendor, activity, format::render_worktree_column(claim_row->worktree_path), branch,
                               format::render_relative_heartbeat(now, claim_row->last_heartbeat_at)));
        rendered_claim = true;
      }
    }
    if (!rendered_claim) {
      // An action with no claim — or whose claim row has since been
      // deleted — still gets a line, so the forest's SHAPE survives.
      out.append(std::format("session:{}  (no claim)\n", node.session_id));
    }
  }

  ctx.out() << out;
  return {};
}

} // namespace planar::cmd::watch::handlers
