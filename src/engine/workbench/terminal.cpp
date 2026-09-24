/// @file terminal.cpp
/// @brief Implementation of `planar.engine.workbench.terminal` (plan 996,
/// task 6037). See terminal.cppm for the two modes and the pass-through rule.

module planar.engine.workbench.terminal;

import std;

namespace planar::engine::workbench::terminal {

namespace {

using cls = classification;

/// @brief One kind's status table: the exact strings its migration CHECK
/// constraint allows, each paired with its classification.
///
/// Keeping the string and the classification in ONE table is what makes
/// `statuses_of` and `classify` impossible to drift apart — they read the
/// same rows.
struct status_row {
  std::string_view name;
  cls              value;
};

constexpr std::array<status_row, 5> k_plan{{
    {"draft", cls::active},
    {"active", cls::active},
    {"paused", cls::active},
    {"done", cls::success_terminal},
    {"abandoned", cls::failure_terminal},
}};

constexpr std::array<status_row, 5> k_task{{
    {"todo", cls::active},
    {"doing", cls::active},
    {"blocked", cls::active},
    {"done", cls::success_terminal},
    {"cancelled", cls::failure_terminal},
}};

constexpr std::array<status_row, 4> k_decision{{
    {"proposed", cls::active},
    {"accepted", cls::active},
    {"superseded", cls::failure_terminal},
    {"withdrawn", cls::failure_terminal},
}};

constexpr std::array<status_row, 3> k_question{{
    {"open", cls::active},
    {"answered", cls::success_terminal},
    {"wontfix", cls::failure_terminal},
}};

constexpr std::array<status_row, 5> k_test_scenario{{
    {"draft", cls::active},
    {"ready", cls::active},
    {"verified", cls::success_terminal},
    // `failing` is ACTIVE, not a failure terminal. A failing scenario is
    // live work that still needs its file on disk; the "failure" in
    // `failure_terminal` means abandoned, not red.
    {"failing", cls::active},
    {"retired", cls::failure_terminal},
}};

constexpr std::array<status_row, 4> k_artifact{{
    {"draft", cls::active},
    {"active", cls::active},
    {"superseded", cls::failure_terminal},
    {"retired", cls::failure_terminal},
}};

auto rows_for(kind entity_kind) -> std::span<const status_row> {
  switch (entity_kind) {
  case kind::plan:
    return k_plan;
  case kind::task:
    return k_task;
  case kind::decision:
    return k_decision;
  case kind::question:
    return k_question;
  case kind::test_scenario:
    return k_test_scenario;
  case kind::artifact:
    return k_artifact;
  }
  return {};
}

} // namespace

auto kind_from_string(std::string_view text) -> std::optional<kind> {
  if (text == "plan") {
    return kind::plan;
  }
  if (text == "task") {
    return kind::task;
  }
  if (text == "decision") {
    return kind::decision;
  }
  if (text == "question") {
    return kind::question;
  }
  // Both spellings, deliberately: `test_scenario` is the table name and
  // `scenario` is what the sync layer's entity stream carries.
  if (text == "test_scenario" || text == "scenario") {
    return kind::test_scenario;
  }
  if (text == "artifact") {
    return kind::artifact;
  }
  return std::nullopt;
}

auto mode_from_string(std::string_view text) -> std::optional<mode> {
  if (text == "failures") {
    return mode::failures;
  }
  if (text == "all") {
    return mode::all;
  }
  return std::nullopt;
}

auto mode_to_string(mode value) -> std::string_view {
  return value == mode::all ? "all" : "failures";
}

auto is_filtered(classification value, mode filter) -> bool {
  switch (value) {
  case classification::active:
    return false;
  case classification::failure_terminal:
    return true;
  case classification::success_terminal:
    return filter == mode::all;
  }
  return false;
}

auto classify(kind entity_kind, std::string_view status_text) -> std::optional<classification> {
  for (auto const& row : rows_for(entity_kind)) {
    if (row.name == status_text) {
      return row.value;
    }
  }
  return std::nullopt;
}

auto statuses_of(kind entity_kind) -> std::span<const std::string_view> {
  // Built once per kind, in table order, so the exhaustiveness test walks
  // exactly the rows `classify` reads.
  static const std::array<std::vector<std::string_view>, 6> k_names = [] {
    std::array<std::vector<std::string_view>, 6> out;
    auto const                                   fill = [&](std::size_t slot, std::span<const status_row> rows) {
      for (auto const& row : rows) {
        out.at(slot).push_back(row.name);
      }
    };
    fill(0, k_plan);
    fill(1, k_task);
    fill(2, k_decision);
    fill(3, k_question);
    fill(4, k_test_scenario);
    fill(5, k_artifact);
    return out;
  }();
  return k_names.at(static_cast<std::size_t>(entity_kind));
}

auto is_filtered_str(std::string_view kind_text, std::string_view status_text, mode filter) -> std::optional<bool> {
  auto const parsed_kind = kind_from_string(kind_text);
  if (!parsed_kind) {
    return std::nullopt;
  }
  auto const value = classify(*parsed_kind, status_text);
  if (!value) {
    return std::nullopt;
  }
  return is_filtered(*value, filter);
}

} // namespace planar::engine::workbench::terminal
