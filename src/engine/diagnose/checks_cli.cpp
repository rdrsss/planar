/// @file checks_cli.cpp
/// @brief The CLI and failure-cluster family of the diagnose catalog (see diagnose.cppm).

module;

module planar.engine.diagnose;

import std;

namespace planar::engine::diagnose::detail {

auto cli_family() -> family {
  return family{};
}

} // namespace planar::engine::diagnose::detail
