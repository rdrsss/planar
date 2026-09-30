/// @file rule.cpp
/// @brief Implementation of `planar.cmd.planar_agent.handlers.queue.rule`.
/// See rule.cppm for the contract.

module planar.cmd.planar_agent.handlers.queue.rule;

import std;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;
import planar.engine.hostqueue.rule;

namespace planar::cmd::agent::handlers {

auto queue_rule(context& ctx, const cliapp::parsed_args& /*args*/) -> handler_outcome {
  auto const text = engine::hostqueue::queue_rule_text();
  ctx.out().write(text.data(), static_cast<std::streamsize>(text.size()));
  return handler_result{};
}

} // namespace planar::cmd::agent::handlers
