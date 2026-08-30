/// @file introspection_adapters.cppm
/// @brief `planar.engine.introspection_adapters` — the no-write privacy
/// boundary that normalizes vendor transcript JSONL into bounded aggregate
/// evidence for `planar report` (plan 996, task 6102).
///
/// Behavior-preserving port (D2) of the IN-MEMORY half of
/// zig/src/engine/introspection_adapters.zig: `collectPreview`, the four
/// per-vendor `extract*` recognizers, the deduplication/sort/warning
/// pipeline, and `discover`. `collectPreview` consumes already-loaded
/// vendor JSONL and returns only bounded aggregate evidence — source text
/// and parsed JSON never outlive the call, and this half of the module has
/// no filesystem or database handle at all.
///
/// ## WHAT IS NOT HERE, AND WHY
///
/// The Zig original's OTHER half —
/// `collectPreviewFromPaths`/`collectVendorPath`/the `FsSeam` fault-
/// injection seam/`TranscriptConfig`/`CliLogAdapter`/`collectConfiguredPreview`/
/// `CollectorLimits` (zig:178) and its three `pub const default_max_*`
/// values (zig:11-13: `default_max_files`, `default_max_bytes`, and
/// `default_max_records` — the last of which THIS half already needs, see
/// `k_default_max_records` below) — (roughly 280 of the file's ~836
/// implementation lines) — walks `~/.claude/projects`, `~/.codex/sessions`,
/// `~/.copilot/session-state`, and a caller-supplied CLI-log reader,
/// applies file/byte/record caps, and hands the result to
/// `collectPreview`. It is disk-discovery PLUMBING for the `report`
/// handler, not part of the collector's own contract, and `report` is not
/// wired this cycle (it also needs `engine/introspect.zig`'s DB-aggregate
/// half, ~1040 implementation lines, entirely unstarted). Porting the
/// discovery layer now would add a filesystem-fault-injection seam with no
/// caller to exercise it against. Left as the named follow-up alongside
/// `engine/introspect.zig` and the handler wiring itself — see task 6102's
/// tracking row.
///
/// ## Why this is a self-contained layer-2 bucket
///
/// One dependency, `planar.json_dom` (layer 1) — for parsing each JSONL
/// line the way `std.json.parseFromSlice(std.json.Value, ...)` does. No
/// `db` dependency: unlike every other module `report` will eventually
/// need, this half touches no table.
module;

export module planar.engine.introspection_adapters;

import std;
import planar.json_dom;

namespace planar::engine::introspection_adapters {

/// @brief A preview cannot carry more distinct evidence buckets than this.
export inline constexpr std::size_t k_max_evidence_buckets = 1024;

/// @brief The oracle's `default_max_records` (zig:13, `pub`). SHARED
/// across two call sites there: `extractClaude`'s pending tool_use pairing
/// cap (zig:526, what THIS half uses it for) and `collectVendorPath`'s
/// per-source record cap (zig:307, the deferred discovery half's job —
/// see this file's header). Exported and named after the oracle constant
/// rather than kept private specifically so a future discovery-half port
/// reaches for THIS symbol instead of re-declaring a same-valued private
/// one under a different name, which would silently decouple the two caps
/// the oracle keeps coupled.
export inline constexpr std::size_t k_default_max_records = 50'000;

/// @brief The four recognized transcript/log sources.
export enum class vendor : std::uint8_t { claude, codex, copilot, cli_log };

/// @brief The four evidence categories a normalized record can carry.
export enum class category : std::uint8_t { failure, retry, abandonment, gap };

/// @brief Per-source coverage state.
export enum class coverage_state : std::uint8_t { observed, unavailable, disabled };

/// @brief Why a warning was raised.
export enum class warning_kind : std::uint8_t {
  unavailable,
  disabled,
  malformed,
  file_cap,
  byte_cap,
  record_cap,
  evidence_cap,
  cli_adapter_failed,
};

/// @brief One coverage- or cap-driven warning.
export struct warning_row {
  vendor        v;         ///< The affected vendor.
  warning_kind  kind;      ///< Why it was raised.
  std::uint32_t count = 1; ///< Occurrence count (1 for a state warning).
};

/// @brief One aggregated evidence bucket: (vendor, verb_path, category,
/// hour bucket of `first_seen`) collapsed to a count and a seen range.
export struct signal_row {
  vendor        v;          ///< The source vendor.
  std::string   verb_path;  ///< The bounded, redacted verb path.
  category      cat;        ///< The evidence category.
  std::uint32_t count = 0;  ///< How many raw records collapsed into this bucket.
  std::string   first_seen; ///< Earliest timestamp merged into this bucket.
  std::string   last_seen;  ///< Latest timestamp merged into this bucket.
};

/// @brief One source's scan tally.
export struct coverage_row {
  vendor         v;                                     ///< The vendor.
  coverage_state state      = coverage_state::observed; ///< Overall state.
  std::uint32_t  scanned    = 0;                        ///< Lines examined.
  std::uint32_t  malformed  = 0;                        ///< Lines that failed to parse or violated the recognized envelope.
  std::uint32_t  normalized = 0;                        ///< Lines that produced (or merged into) a signal.
  std::uint32_t  ignored    = 0;                        ///< Lines that parsed but carried no evidence.
  std::uint32_t  capped     = 0;                        ///< Lines that would have produced a new bucket past the evidence cap.
};

/// @brief One discovered source. `jsonl` may contain multiple raw vendor
/// records, one JSON object per line.
export struct raw_source {
  vendor      v;                ///< Which vendor this source came from.
  bool        enabled   = true; ///< Whether the adapter is configured on.
  bool        available = true; ///< Whether the source could be read at all.
  std::string jsonl;            ///< Raw newline-delimited JSON. Empty when unavailable/disabled.
};

/// @brief Redacted preview output. Owns only normalized keys and
/// timestamps — never raw transcript prose, arguments, or entity text.
export struct preview {
  std::vector<signal_row>   signals;  ///< Aggregated evidence buckets, deduplicated and sorted.
  std::vector<coverage_row> coverage; ///< Per-vendor scan tallies, sorted by vendor.
  std::vector<warning_row>  warnings; ///< Coverage- and cap-driven warnings.
};

/// @brief Collect a no-write preview from raw vendor JSONL.
///
/// Invalid JSON and malformed recognized envelopes are isolated per line
/// (`coverage.malformed`) without failing the whole source. Structurally
/// valid but uninteresting record kinds are ignored without evidence
/// (`coverage.ignored`). A source past `enabled`/`available` contributes a
/// coverage row and nothing else. `signals` is deduplicated (an
/// authoritative `cli_log` bucket suppresses any transcript-vendor twin
/// with the same verb path/category/hour bucket) and sorted deterministically
/// by (vendor, verb_path, category, first_seen); `coverage` is sorted by
/// vendor. Neither order depends on `sources`' order or any hash-map
/// iteration.
/// @param sources The raw per-vendor JSONL to normalize.
/// @return The aggregated, redacted preview.
export auto collect_preview(std::span<const raw_source> sources) -> preview;

/// @brief Resolve which path a vendor should read: `builtin` when enabled
/// and no override is set, `override_path` when both enabled and set, or
/// absent when the vendor is disabled. Disabled wins over both.
/// @param enabled Whether the vendor's adapter is configured on.
/// @param override_path The operator override, or empty for none.
/// @param builtin The built-in default path.
/// @return The resolved path, or unset when disabled.
export auto discover(bool enabled, std::string_view override_path, std::string_view builtin) -> std::optional<std::string_view>;

} // namespace planar::engine::introspection_adapters
