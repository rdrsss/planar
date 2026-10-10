// @file thresholds.t.cpp
// @brief Pins the shared cutoffs of `planar.core.thresholds`.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.core.thresholds;

TEST_CASE("the stale-handoff threshold is 24 hours", "[core][thresholds]") {
  CHECK(planar::core::stale_handoff_threshold_hours == 24);
}
