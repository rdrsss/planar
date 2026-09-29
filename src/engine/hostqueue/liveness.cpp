/// @file liveness.cpp
/// @brief Implementation of `planar.engine.hostqueue.liveness` (plan 1080,
/// task hq-liveness). See liveness.cppm for the contract.

module planar.engine.hostqueue.liveness;

import std;
import planar.process.identity;
import planar.engine.hostqueue.queue;

namespace planar::engine::hostqueue {

auto system_process_probe() -> process_probe {
  return process_probe{};
}

auto is_fresh(std::int64_t /*refreshed_mono*/, const liveness_context& /*ctx*/) -> bool {
  return true;
}

auto judge_liveness(const entry& /*e*/, const liveness_context& /*ctx*/, const process_probe& /*probe*/)
    -> std::expected<liveness, process::identity::error> {
  return liveness{.live = true, .submitter_live = true, .group = group_verdict::not_checked};
}

auto submitter_gone(const entry& e, const liveness_context& ctx, const process_probe& probe)
    -> std::expected<bool, process::identity::error> {
  return judge_liveness(e, ctx, probe).transform([](const liveness& l) { return l.submitter_gone(); });
}

auto select_not_live(std::span<entry const> /*entries*/, const liveness_context& /*ctx*/, const process_probe& /*probe*/)
    -> std::vector<std::int64_t> {
  return {};
}

} // namespace planar::engine::hostqueue
