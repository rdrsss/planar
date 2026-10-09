/// @file checks_queue.cpp
/// @brief The queue family of the diagnose catalog (see diagnose.cppm).

module;

module planar.engine.diagnose;

import std;
import planar.incident_model;

namespace planar::engine::diagnose::detail {

auto queue_family() -> family {
  family f;
  // The run-scope input (`--run`, M3): declared now so every run reports it `not_applicable`
  // with reason `check-not-built`. Its reader and scoping arrive with the run-identity task.
  f.inputs.push_back(input_def{.name = "run_identity", .probe = {}, .built = false});
  f.inputs.push_back(input_def{.name = "queue_observation", .probe = {}, .built = false});
  f.checks.push_back(check_def{.id       = "queue-ended-unobserved",
                               .kind     = incident_model::check_kind::state,
                               .severity = incident_model::diagnostic_severity::warning,
                               .category = "queue_unobserved",
                               .recovery = "planar-agent queue ack <seq> after reading the result",
                               .inputs   = {"queue_observation"},
                               .built    = false,
                               .evaluate = {}});
  return f;
}

} // namespace planar::engine::diagnose::detail
