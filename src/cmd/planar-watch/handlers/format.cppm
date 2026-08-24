/// @file format.cppm
/// @brief `planar.cmd.planar_watch.handlers.format` — the column
/// formatters and the clock the read verbs share (plan 996, task 6120).
///
/// Port target: zig/src/cmd/planar-watch/handlers/format.zig, plus
/// `ps.zig`'s `writeNowIso` and the `relativeTime` / `isoToMs` pair
/// `follow.zig` owns.
///
/// ## Why these are here and not at layer 2
///
/// Every function below is PURE TEXT: no SQLite handle, no engine type
/// beyond a `std::string_view`. They are display policy for one binary's
/// text columns — how wide the activity column is, which basename survives
/// a long worktree path, whether a heartbeat reads `7s` or `2h`. Pushing
/// them into `engine_runtime` would put `planar-watch`'s rendering choices
/// in a library `planar-agent` also links, and nothing there wants them.
/// The Zig tree draws the same line: `format.zig` sits in the handlers
/// directory, not in `engine/runtime/agentactivity/`.
///
/// The extraction from `ps` into a shared module is also zig's, and for
/// zig's stated reason: `tree` renders the same claim columns and reaching
/// into `ps.zig` for them created an import cycle.
///
/// ## The clock is NOT SQLite's here, and that is a real difference
///
/// Every timestamp `planar-watch` READS came from
/// `strftime('%Y-%m-%dT%H:%M:%fZ','now')` inside SQLite. The two values
/// this module PRODUCES from a clock — `generated_at` and the `last_hb:`
/// relative column — come from the host clock instead, because the
/// reference binary takes them from `std.Io.Clock.now(.real)` rather than
/// asking the database. Reproduced (D2). It is also why no test in this
/// tree asserts a literal `generated_at` value: the field is
/// nondeterministic by construction, and a test that pinned it would be
/// pinning the moment it was written.
module;

export module planar.cmd.planar_watch.handlers.format;

import std;

namespace planar::cmd::watch::handlers::format {

/// @brief Milliseconds since the UNIX epoch, from the host clock.
/// @return The current wall-clock time in milliseconds.
export auto now_ms() -> std::int64_t;

/// @brief The current UTC time in SQLite's `%Y-%m-%dT%H:%M:%fZ` shape.
///
/// Rendered to match the row timestamps byte-for-byte in FORM (fixed
/// width, millisecond precision, trailing `Z`) so a consumer can compare
/// `generated_at` against a `claimed_at` with a plain string compare.
/// @return The timestamp, WITHOUT surrounding quotes.
export auto now_iso() -> std::string;

/// @brief Parse SQLite's ISO-8601 UTC form to milliseconds since epoch.
///
/// Accepts both widths the tables actually hold:
/// `YYYY-MM-DDTHH:MM:SS.mmmZ` and `YYYY-MM-DDTHH:MM:SSZ`. Anything else —
/// wrong length, missing `Z`, a non-digit where a digit belongs — is unset
/// rather than a guess, because a timestamp this function guessed at would
/// surface as a plausible-looking but wrong `last_hb:` column.
/// @param iso The timestamp text.
/// @return Milliseconds since epoch, or unset when unparseable.
export auto iso_to_ms(std::string_view iso) -> std::optional<std::int64_t>;

/// @brief Render `then_iso` as an age relative to `now_ms_value`.
///
/// The buckets are zig's, boundaries included: under five seconds (AND any
/// FUTURE timestamp, which is why the comparison is signed) is
/// `"just now"`; then `Ns ago` under a minute, `Nm ago` under an hour,
/// `Nh ago` under a day, `Nd ago` beyond.
/// @param now_ms_value The reference instant, in milliseconds since epoch.
/// @param then_iso The timestamp to age.
/// @return The rendered age, or unset when `then_iso` is unparseable.
export auto relative_time(std::int64_t now_ms_value, std::string_view then_iso) -> std::optional<std::string>;

/// @brief Render the `activity:` column body — the latest action's summary,
/// quoted.
///
/// Empty or absent renders `""`. A summary at or under 80 BYTES is quoted
/// whole. Beyond that it is cut to at most 77 bytes and closed with U+2026,
/// so the quoted result never exceeds 82 bytes — and the cut walks BACK off
/// any UTF-8 continuation byte first, so truncation cannot split a
/// multi-byte character and emit invalid UTF-8 into a terminal.
/// @param summary The action summary, or unset.
/// @return The column body, quotes included.
export auto render_activity_summary(const std::optional<std::string>& summary) -> std::string;

/// @brief Render the `worktree:` column body.
///
/// Absent or empty renders `""` (quoted, unlike the populated case).
/// A path at or under 40 characters renders as its BASENAME alone; a longer
/// one renders as U+2026 followed by the basename, so the operator can tell
/// "short path, shown fully qualified by its last segment" from "path
/// elided". The 40-character test is against the FULL path, not the
/// basename.
/// @param worktree_path The claim's worktree path, or unset.
/// @return The column body.
export auto render_worktree_column(const std::optional<std::string>& worktree_path) -> std::string;

/// @brief Render the `last_hb:` column body.
///
/// `relative_time` with the `" ago"` suffix stripped, because the column
/// key already supplies that context. An empty or unparseable timestamp
/// renders as the EMPTY STRING — the column key stays, its value does not,
/// which is what the reference binary does.
/// @param now_ms_value The reference instant, in milliseconds since epoch.
/// @param last_heartbeat_at The claim's heartbeat timestamp.
/// @return The column body.
export auto render_relative_heartbeat(std::int64_t now_ms_value, std::string_view last_heartbeat_at) -> std::string;

} // namespace planar::cmd::watch::handlers::format
