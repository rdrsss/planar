/// @file roles.cpp
/// @brief Implementation of `planar.engine.models.roles`.

module planar.engine.models.roles;

import std;
import planar.engine.models.registry;
import planar.engine.models.ranking;
import planar.engine.models.profile;

namespace planar::engine::models::roles {

namespace {

using namespace std::string_view_literals;

/// The role table, in enum order. `wire` is the EMITTED (underscored)
/// spelling, which is what `@tagName` produces in the oracle.
struct role_row {
  role             value;
  std::string_view wire;
  packet_class     class_;
};

constexpr std::array role_rows{
    role_row{role::planner, "planner"sv, packet_class::planning},
    role_row{role::spec_reviewer, "spec_reviewer"sv, packet_class::planning},
    role_row{role::ingestor, "ingestor"sv, packet_class::planning},
    role_row{role::orchestrator, "orchestrator"sv, packet_class::planning},
    role_row{role::coder, "coder"sv, packet_class::task},
    role_row{role::test_coder, "test_coder"sv, packet_class::task},
    role_row{role::reviewer, "reviewer"sv, packet_class::task},
    role_row{role::research, "research"sv, packet_class::task},
    role_row{role::janitor, "janitor"sv, packet_class::task},
};

/// Build a resolution that rests on nothing, naming why.
[[nodiscard]] auto fallback_for(role value, packet_class class_, static_fallback fallback, fallback_reason reason) -> resolution {
  return resolution{
      .role_            = value,
      .class_           = class_,
      .source_          = source::static_fallback,
      .tier_            = fallback.tier_,
      .work_type_       = std::nullopt,
      .complexity_      = std::nullopt,
      .fallback_reason_ = reason,
      .rule_version     = std::nullopt,
  };
}

} // namespace

auto role_from_wire(std::string_view text) -> std::optional<role> {
  // The oracle copies the flag into a fixed 64-byte buffer and refuses a
  // longer name BEFORE the lookup. Reproduced so an over-long role is a
  // distinct refusal rather than a silent miss.
  if (text.size() > 64) {
    return std::nullopt;
  }
  std::string normalized{text};
  std::ranges::replace(normalized, '-', '_');
  auto it = std::ranges::find(role_rows, normalized, &role_row::wire);
  if (it == role_rows.end()) {
    return std::nullopt;
  }
  return it->value;
}

auto role_to_text(role value) -> std::string_view {
  auto it = std::ranges::find(role_rows, value, &role_row::value);
  return it == role_rows.end() ? std::string_view{} : it->wire;
}

auto class_of(role value) -> packet_class {
  auto it = std::ranges::find(role_rows, value, &role_row::value);
  return it == role_rows.end() ? packet_class::task : it->class_;
}

auto packet_class_to_text(packet_class value) -> std::string_view {
  switch (value) {
  case packet_class::planning:
    return "planning"sv;
  case packet_class::task:
    return "task"sv;
  }
  return {};
}

auto source_to_text(source value) -> std::string_view {
  switch (value) {
  case source::packet:
    return "packet"sv;
  case source::static_fallback:
    return "static_fallback"sv;
  }
  return {};
}

auto fallback_reason_to_text(fallback_reason value) -> std::string_view {
  switch (value) {
  case fallback_reason::no_packet:
    return "no_packet"sv;
  case fallback_reason::packet_not_ready:
    return "packet_not_ready"sv;
  case fallback_reason::policy_not_ready:
    return "policy_not_ready"sv;
  }
  return {};
}

auto packet_backed(const resolution& value) -> bool {
  return value.source_ == source::packet;
}

auto resolve_task(role value, const std::optional<profile::outcome>& outcome, static_fallback fallback) -> resolution {
  if (!outcome.has_value()) {
    return fallback_for(value, packet_class::task, fallback, fallback_reason::no_packet);
  }
  return std::visit(
      [&](const auto& alternative) -> resolution {
        using alt = std::decay_t<decltype(alternative)>;
        if constexpr (std::is_same_v<alt, profile::profile>) {
          return resolution{
              .role_            = value,
              .class_           = packet_class::task,
              .source_          = source::packet,
              .tier_            = alternative.tier_floor,
              .work_type_       = alternative.type,
              .complexity_      = alternative.risk,
              .fallback_reason_ = std::nullopt,
              .rule_version     = profile::rule_version,
          };
        } else if constexpr (std::is_same_v<alt, profile::not_ready_reason>) {
          return fallback_for(value, packet_class::task, fallback, fallback_reason::packet_not_ready);
        } else {
          return fallback_for(value, packet_class::task, fallback, fallback_reason::policy_not_ready);
        }
      },
      *outcome);
}

auto resolve_task_packet(role value, bool packet_ready, const std::optional<profile::outcome>& outcome, static_fallback fallback)
    -> resolution {
  if (!packet_ready) {
    return fallback_for(value, packet_class::task, fallback, fallback_reason::packet_not_ready);
  }
  return resolve_task(value, outcome, fallback);
}

auto resolve_planning(role value, std::optional<bool> packet_ready, static_fallback fallback,
                      std::string_view packet_policy_version) -> resolution {
  if (!packet_ready.has_value()) {
    return fallback_for(value, packet_class::planning, fallback, fallback_reason::no_packet);
  }
  if (!*packet_ready) {
    return fallback_for(value, packet_class::planning, fallback, fallback_reason::packet_not_ready);
  }
  // A ready planning packet still routes at the CONFIGURED tier — there is no
  // work to classify — but it is packet-BACKED, and that is the difference the
  // operator is being shown.
  return resolution{
      .role_            = value,
      .class_           = packet_class::planning,
      .source_          = source::packet,
      .tier_            = fallback.tier_,
      .work_type_       = std::nullopt,
      .complexity_      = std::nullopt,
      .fallback_reason_ = std::nullopt,
      .rule_version     = packet_policy_version,
  };
}

} // namespace planar::engine::models::roles
