/// @file groups.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.groups`.

module planar.cmd.planar.handlers.groups;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.grouping;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace load = engine::grouping::load;

auto groups_recommend(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const raw_plan = cliapp::positional_string(args, "plan-id").value_or(std::string{});
  auto const plan_id  = cliapp::parse_int64_zig(raw_plan);
  if (!plan_id.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, load::render_invalid_plan_id(raw_plan)));
  }

  // `--budget` is a STRING in the tree, not an int, because the oracle parses
  // it as an unsigned 32-bit value and refuses with its own wording. Declaring
  // it as an int would hand `-1` and `4294967296` to CLI11, whose refusal is a
  // different message at a different exit code. The two out-of-range shapes
  // are why the check is `parse_uint64_zig` PLUS an explicit u32 ceiling:
  // Zig's `parseInt(u32, ...)` rejects an overflow, it does not truncate.
  auto budget = load::k_default_budget;
  if (auto const raw = cliapp::flag_string(args, "--budget"); raw.has_value()) {
    auto const parsed = cliapp::parse_uint64_zig(*raw);
    if (!parsed.has_value() || *parsed > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, load::render_invalid_budget(*raw)));
    }
    budget = static_cast<std::uint32_t>(*parsed);
  }

  // Forwarded to `load::recommend_with` (task 6460). The recommendation's
  // `solver` field reports the solver that ACTUALLY ran — a
  // `--solver mtkahypar` run degrades to `greedy` with
  // `optimal_available:false` when the seam is unavailable (build not linked
  // with `-DPLANAR_WITH_MTKAHYPAR=ON`, or the call itself failed), which is
  // byte-identical to what the oracle produces on a host with no solver
  // installed. An unknown value is still a refusal. See groups.cppm.
  auto requested_solver = load::solver::greedy;
  if (auto const raw = cliapp::flag_string(args, "--solver"); raw.has_value()) {
    auto const parsed = load::solver_from_text(*raw);
    if (!parsed.has_value()) {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, load::render_invalid_solver(*raw)));
    }
    requested_solver = *parsed;
  }

  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const rec = load::recommend_with(**conn, *plan_id, budget, requested_solver);
  if (!rec) {
    // `not_found` is exit 1, NOT exit 2: the oracle folds entity-not-found
    // into the generic bucket so a script can `|| exit 1` cleanly. A plan that
    // EXISTS with no open tasks is a success with zero slices, never this.
    if (rec.error() == load::grouping_error::not_found) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, load::render_plan_not_found(*plan_id)));
    }
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "groups recommend: QueryFailed"));
  }

  ctx.out() << (cliapp::flag_bool(args, "--json") ? load::render_json(*rec) : load::render_text(*rec));
  return {};
}

} // namespace planar::cmd::handlers
