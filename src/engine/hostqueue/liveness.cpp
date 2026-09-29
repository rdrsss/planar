/// @file liveness.cpp
/// @brief Implementation of `planar.engine.hostqueue.liveness` (plan 1080,
/// task hq-liveness). See liveness.cppm for the contract.

module planar.engine.hostqueue.liveness;

import std;
import planar.process.identity;
import planar.engine.hostqueue.queue;

namespace planar::engine::hostqueue {

namespace {

namespace identity = process::identity;

/// @brief Whether the checker may test this entry's process ids: both host
/// identities are known and equal.
auto same_host(const entry& e, const liveness_context& ctx) -> bool {
  return e.host_id == ctx.host_id && e.host_id != identity::k_unknown_host_identity;
}

/// @brief The `waiting` test for an entry on the checker's own host.
auto submitter_passes(const entry& e, const liveness_context& ctx, const process_probe& probe)
    -> std::expected<bool, identity::error> {
  auto exists = probe.process_exists(e.pid);
  if (!exists) {
    return std::unexpected(exists.error());
  }
  if (!*exists) {
    return false;
  }
  auto started = probe.process_start_time(e.pid);
  if (!started) {
    return std::unexpected(started.error());
  }
  if (!started->has_value() || **started != static_cast<identity::start_time>(e.pid_started)) {
    return false;
  }
  return is_fresh(e.refreshed_mono, ctx);
}

/// @brief The child group's state for a running entry on the checker's host.
auto judge_group(const entry& e, const process_probe& probe) -> std::expected<group_verdict, identity::error> {
  if (e.state != entry_state::running || !e.child_pgid) {
    return group_verdict::not_checked;
  }
  auto const pgid = *e.child_pgid;
  if (e.child_started) {
    auto leader = probe.process_start_time(pgid);
    if (!leader) {
      return std::unexpected(leader.error());
    }
    if (leader->has_value() && **leader != static_cast<identity::start_time>(*e.child_started)) {
      return group_verdict::reused;
    }
  }
  auto members = probe.group_has_members(pgid);
  if (!members) {
    return std::unexpected(members.error());
  }
  return *members ? group_verdict::has_members : group_verdict::empty;
}

} // namespace

auto system_process_probe() -> process_probe {
  return process_probe{
      .process_exists     = [](std::int64_t pid) { return identity::process_exists(pid); },
      .process_start_time = [](std::int64_t pid) { return identity::process_start_time(pid); },
      .group_has_members  = [](std::int64_t pgid) { return identity::group_has_members(pgid); },
  };
}

auto is_fresh(std::int64_t refreshed_mono, const liveness_context& ctx) -> bool {
  // The distance is taken in unsigned arithmetic so no pair of values
  // overflows; the window is never negative in a sane configuration, and a
  // negative one makes nothing fresh.
  if (ctx.stale_after_ms < 0) {
    return false;
  }
  auto const now      = static_cast<std::uint64_t>(ctx.now_mono);
  auto const then     = static_cast<std::uint64_t>(refreshed_mono);
  auto const distance = ctx.now_mono >= refreshed_mono ? now - then : then - now;
  return distance <= static_cast<std::uint64_t>(ctx.stale_after_ms);
}

auto judge_liveness(const entry& e, const liveness_context& ctx, const process_probe& probe)
    -> std::expected<liveness, process::identity::error> {
  if (!same_host(e, ctx)) {
    bool const fresh = is_fresh(e.refreshed_mono, ctx);
    return liveness{.live = fresh, .submitter_live = fresh, .group = group_verdict::not_checked};
  }

  auto submitter = submitter_passes(e, ctx, probe);
  if (!submitter) {
    return std::unexpected(submitter.error());
  }
  auto group = judge_group(e, probe);
  if (!group) {
    return std::unexpected(group.error());
  }

  bool const live = *submitter || (e.state == entry_state::running && *group == group_verdict::has_members);
  return liveness{.live = live, .submitter_live = *submitter, .group = *group};
}

auto judge_child_group(const entry& e, const process_probe& probe) -> std::expected<group_verdict, process::identity::error> {
  return judge_group(e, probe);
}

auto submitter_gone(const entry& e, const liveness_context& ctx, const process_probe& probe)
    -> std::expected<bool, process::identity::error> {
  return judge_liveness(e, ctx, probe).transform([](const liveness& verdict) { return verdict.submitter_gone(); });
}

auto select_not_live(std::span<entry const> entries, const liveness_context& ctx, const process_probe& probe)
    -> std::vector<std::int64_t> {
  std::vector<std::int64_t> selected;
  for (auto const& e : entries) {
    auto verdict = judge_liveness(e, ctx, probe);
    if (verdict && !verdict->live) {
      selected.push_back(e.seq);
    }
  }
  return selected;
}

} // namespace planar::engine::hostqueue
