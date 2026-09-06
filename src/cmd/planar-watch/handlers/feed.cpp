/// @file feed.cpp
/// @brief Implementation of the read-only `planar-watch feed` activity view,
/// including the `--follow` streaming arm (plan 1006, task 6449).
module planar.cmd.planar_watch.handlers.feed;
import std;
import planar.cliapp.args;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentrender;
import planar.json_text;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.exit;
import planar.cmd.planar_watch.handlers.follow;
namespace planar::cmd::watch::handlers {
namespace aa = engine::runtime::agentactivity;
namespace ar = engine::runtime::agentrender;
namespace {

auto failure(aa::agent_error e) -> domain_error {
  return error_from_body(domain_error_kind::generic_failure, std::format("feed: {}", aa::error_name(e)));
}

struct event {
  std::string at, kind, body;
};

/// @brief Collect the events matching `args`' filters, restricted to those
/// with `at` strictly greater than `watermark` (when supplied). Sorted
/// ascending by `at`. Re-acquires `ctx`'s DB handle on every call so a
/// follow-loop caller sees whatever `interruptible_sleep` last refreshed.
///
/// Wraps the two SELECTs in one read transaction so they share a
/// consistent SQLite snapshot -- without the wrap each statement gets its
/// own implicit read transaction, and a concurrent writer can commit
/// between them (surfacing as "action row visible but its matching claim
/// row is not", or vice versa). Ported from the oracle's
/// `feed.zig:collectBetween`; best-effort (a BEGIN failure just falls back
/// to the unwrapped per-statement behavior rather than refusing service).
auto collect_events(context& ctx, const cliapp::parsed_args& args, std::optional<std::string> const& watermark)
    -> std::expected<std::vector<event>, domain_error> {
  auto conn = ctx.ensure_db();
  if (!conn)
    return std::unexpected(conn.error());
  bool const have_tx = (*conn)->execute("BEGIN DEFERRED").has_value();
  auto       rows    = aa::list_actions(**conn, 4096);
  auto       claims  = aa::list_claims(**conn, aa::claim_status_filter::all);
  if (have_tx)
    static_cast<void>((*conn)->execute("COMMIT"));
  if (!rows)
    return std::unexpected(failure(rows.error()));
  if (!claims)
    return std::unexpected(failure(claims.error()));

  std::vector<event> events;
  auto                include = [&](std::string_view at, std::string_view vendor, std::optional<std::int64_t> task) {
    if (auto filter = cliapp::flag_string(args, "--vendor"); filter.has_value() && vendor != *filter)
      return false;
    if (auto filter = cliapp::flag_int(args, "--task"); filter.has_value() && task != *filter)
      return false;
    if (auto since = cliapp::flag_string(args, "--since"); since.has_value() && at < *since)
      return false;
    if (watermark.has_value() && at <= *watermark)
      return false;
    return true;
  };
  for (auto const& row : *rows) {
    if (auto plan = cliapp::flag_int(args, "--plan");
        plan.has_value() &&
        (!row.entity || !row.entity_id || !aa::action_belongs_to_plan(**conn, *row.entity, *row.entity_id, *plan)))
      continue;
    auto task = row.entity.has_value() && aa::to_text(*row.entity) == "task" ? row.entity_id : std::nullopt;
    auto add  = [&](std::string_view at, std::string_view kind) {
      if (include(at, row.vendor, task)) {
        std::string body;
        ar::append_action(body, row);
        events.push_back({std::string{at}, std::string{kind}, std::move(body)});
      }
    };
    add(row.started_at, "action_started");
    if (row.ended_at)
      add(*row.ended_at, "action_ended");
  }
  for (auto const& claim : *claims) {
    if (auto plan = cliapp::flag_int(args, "--plan");
        plan.has_value() && !aa::claim_belongs_to_plan(**conn, claim.kind, claim.entity_id, *plan))
      continue;
    auto task = aa::to_text(claim.kind) == "task" ? std::optional{claim.entity_id} : std::nullopt;
    auto add  = [&](std::string_view at, std::string_view kind) {
      if (include(at, claim.vendor, task)) {
        std::string body;
        ar::append_claim_view(body, claim, {});
        events.push_back({std::string{at}, std::string{kind}, std::move(body)});
      }
    };
    add(claim.claimed_at, "claim_acquired");
    if (claim.last_heartbeat_at != claim.claimed_at)
      add(claim.last_heartbeat_at, "heartbeat");
    if (claim.released_at)
      add(*claim.released_at, aa::to_text(claim.status));
  }
  std::ranges::sort(events, {}, &event::at);
  return events;
}

/// @brief Render `events` (NDJSON when `json`, a compact text line
/// otherwise) to `ctx.out()`, flushed immediately.
///
/// The flush is load-bearing under `--follow`: a piped consumer (or a test
/// harness reading the child's stdout) must see each incremental batch as
/// it lands rather than whenever libc++'s buffer happens to fill or the
/// process exits.
auto emit_events(context& ctx, bool json, std::span<event const> events) -> void {
  std::string out;
  for (auto const& e : events) {
    if (json) {
      out.append("{\"event\":");
      json_text::append_json_string(out, e.kind);
      out.append(",\"at\":");
      json_text::append_json_string(out, e.at);
      out.append(e.kind.starts_with("action") ? ",\"action\":" : ",\"claim\":");
      out.append(e.body);
      out.append("}\n");
    } else
      out.append(std::format("  {}  {}\n", e.at, e.kind));
  }
  ctx.out() << out;
  ctx.out().flush();
}

} // namespace

auto feed(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  bool const follow_flag = cliapp::flag_bool(args, "--follow");
  if (follow_flag)
    install_sigint_handler();

  auto const tail = cliapp::flag_int(args, "--tail");
  if (tail.has_value() && *tail <= 0)
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "feed: --tail must be a positive integer"));
  auto const limit = tail.value_or(cliapp::flag_int(args, "--limit").value_or(100));
  bool const json  = cliapp::flag_bool(args, "--json");

  // Initial snapshot: every matching event, then trimmed to the most
  // recent `limit`.
  auto snapshot = collect_events(ctx, args, std::nullopt);
  if (!snapshot)
    return std::unexpected(snapshot.error());
  if (snapshot->size() > static_cast<std::size_t>(limit))
    snapshot->erase(snapshot->begin(), snapshot->end() - static_cast<std::ptrdiff_t>(limit));
  emit_events(ctx, json, *snapshot);

  // Watermark advances to the newest event actually emitted, so the
  // streaming pass below picks up only genuinely NEW rows (no re-emit of
  // the snapshot's own tail).
  std::optional<std::string> watermark;
  if (!snapshot->empty())
    watermark = snapshot->back().at;

  if (!follow_flag)
    return {};

  auto const interval_ns = interval_or_default(cliapp::flag_string(args, "--interval"));

  while (true) {
    if (should_stop())
      return {};
    // Refreshes ctx's read-only DB handle before returning on every path
    // (wake, heartbeat, interrupted) -- see follow.cppm's header for why
    // that refresh is what makes WAL rotation safe to survive.
    interruptible_sleep(ctx, interval_ns);
    if (should_stop())
      return {};

    auto incremental = collect_events(ctx, args, watermark);
    if (!incremental)
      return std::unexpected(incremental.error());
    emit_events(ctx, json, *incremental);
    if (!incremental->empty())
      watermark = incremental->back().at;
  }
}
} // namespace planar::cmd::watch::handlers
