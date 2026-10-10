/// @file thresholds.cppm
/// @brief planar.core.thresholds -- numeric cutoffs that several surfaces must agree on.
///
/// A cutoff lives here when more than one engine bucket applies it and a disagreement would make
/// two commands report different facts about the same row. Each consumer imports the constant
/// instead of restating the number, so a change is one edit.
///
/// Today: the age past which a pending or validated handoff is stale. `planar health`,
/// `planar report` and `planar-watch diagnose` (the `handoff-stale` check) all apply it.
export module planar.core.thresholds;

import std;

namespace planar::core {

/// @brief The age, in hours, beyond which a `pending` or `validated` handoff is stale. A handoff
/// exactly this old is not stale. Applied by `planar health`, `planar report` and the
/// `handoff-stale` diagnose check.
export inline constexpr std::int64_t stale_handoff_threshold_hours = 24;

} // namespace planar::core
