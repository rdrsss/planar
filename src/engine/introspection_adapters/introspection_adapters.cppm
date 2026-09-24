/// @file introspection_adapters.cppm
/// @brief `planar.engine.introspection_adapters` — the no-write privacy
/// boundary that normalizes vendor transcript JSONL into bounded aggregate
/// evidence for `planar report` (plan 996, tasks 6102 and 6352).
///
/// Behavior-preserving port (D2) of BOTH halves of
/// zig/src/engine/introspection_adapters.zig:
///
///   - task 6102 (unchanged this cycle): `collectPreview`, the four
///     per-vendor `extract*` recognizers, the deduplication/sort/warning
///     pipeline, and `discover`. Consumes already-loaded vendor JSONL and
///     returns only bounded aggregate evidence — source text and parsed
///     JSON never outlive the call, and this half has no filesystem or
///     database handle.
///   - task 6352 (this cycle): `collectPreviewFromPaths`/
///     `collectVendorPath`, the fault-injection seam, `TranscriptConfig`,
///     `CliLogAdapter`, and `collectConfiguredPreview`'s glue (see "WHY NO
///     `collect_configured_preview`" below). This half walks
///     `~/.claude/projects`, `~/.codex/sessions`,
///     `~/.copilot/session-state`, and a caller-supplied CLI-log reader,
///     applies file/byte/record caps, and hands the result to
///     `collect_preview`. It is disk-discovery PLUMBING for the `report`
///     handler.
///
/// ## PREVIOUS HEADER SAID THIS WAS LAYER 2, PERMANENTLY — CORRECTED
///
/// Task 6102's original header called `preview` and its four enums
/// "layer-2, part of this bucket's own contract" as a statement about
/// where they would always live. Decision 981 (task 6121's reviewer
/// finding) settled that `engine_introspect` needs the SAME `preview`
/// type and cannot reach it here (`engine_introspect ->
/// engine_introspection_adapters` FATALs D15 at configure time) — so this
/// cycle extracts `vendor`/`category`/`coverage_state`/`warning_kind`/
/// `warning_row`/`signal_row`/`coverage_row`/`preview` to the new layer-1
/// `planar.introspection_preview` module (see its header for the full
/// account) and re-exports them here via `export using` so every existing
/// caller of `introspection_adapters::vendor` etc. (including this
/// bucket's own task-6102 tests) keeps compiling unchanged. `raw_source`
/// and every function stay here — see `planar.introspection_preview`'s
/// header for why.
///
/// ## WHY NO `collect_configured_preview`
///
/// The oracle's `collectConfiguredPreview(allocator, home_dir, transcripts:
/// anytype, cli, limits)` is a thin duck-typed wrapper: Zig's `anytype`
/// lets it accept `engine.config.Effective.Introspection.transcripts`
/// (whatever shape that struct has) with no explicit coupling. C++ has no
/// equivalent without either a template (which would still need the
/// caller's concrete type at the call site, buying nothing over calling
/// `collect_preview_from_paths` directly) or importing
/// `planar.engine.config`'s `transcripts_config` type here — the latter
/// would be an `engine_introspection_adapters -> engine_config` edge,
/// FORBIDDEN by the exact same D15 rule this cycle's whole layer-1
/// extraction exists to route around. So this port stops one level short
/// of the oracle's wrapper: `report`'s layer-3 handler (which may legally
/// import both `engine_config` and `engine_introspection_adapters`) reads
/// `resolved.cfg.introspection.transcripts` and assembles this module's own
/// `transcript_config` directly. Same behavior, one fewer indirection, no
/// forbidden edge.
///
/// That hand-copy at the `report` handler is a field-by-field assignment
/// list the compiler cannot check against "every field got copied" — a
/// new field on either struct would compile clean and silently drop.
/// `report.cpp` guards it with two structured-binding decompositions
/// pinning both structs' exact field counts, so a change to either one
/// fails to compile at that exact spot instead (task 6356).
///
/// ## Why this is a self-contained layer-2 bucket
///
/// Two dependencies: `planar.json_dom` (layer 1) for parsing each JSONL
/// line the way `std.json.parseFromSlice(std.json.Value, ...)` does, and
/// `planar.introspection_preview` (layer 1, this cycle) for the shared
/// preview vocabulary. No `db` dependency: unlike every other module
/// `report` needs, this half touches no table — filesystem only.
module;

export module planar.engine.introspection_adapters;

import std;
import planar.json_dom;
import planar.introspection_preview;

namespace planar::engine::introspection_adapters {

namespace preview_types = planar::introspection_preview;

// --- re-exported layer-1 preview vocabulary (decision 981) ----------------
//
// Kept under this bucket's OWN names via `export using` so every existing
// caller — including introspection_adapters.t.cpp's task-6102 fixtures —
// keeps compiling against `introspection_adapters::vendor` etc. unchanged.
// The canonical definitions now live in `planar.introspection_preview`;
// see that module's header for why each one moved.

export using preview_types::vendor;
export using preview_types::category;
export using preview_types::coverage_state;
export using preview_types::warning_kind;
export using preview_types::warning_row;
export using preview_types::signal_row;
export using preview_types::coverage_row;
export using preview_types::preview;

/// @brief A preview cannot carry more distinct evidence buckets than this.
export inline constexpr std::size_t k_max_evidence_buckets = 1024;

/// @brief The oracle's `default_max_records` (zig:13, `pub`). SHARED
/// across two call sites there: `extractClaude`'s pending tool_use pairing
/// cap (zig:526, what THIS half uses it for) and `collectVendorPath`'s
/// per-source record cap (zig:307, task 6352's `collect_vendor_path`,
/// below). Exported and named after the oracle constant rather than kept
/// private specifically so the discovery-half port (task 6352) reached for
/// THIS symbol instead of re-declaring a same-valued private one under a
/// different name, which would have silently decoupled the two caps the
/// oracle keeps coupled. A reviewer on task 6121 flagged this exact trap
/// as one to avoid; task 6352 reused it as intended.
export inline constexpr std::size_t k_default_max_records = 50'000;

/// @brief The oracle's `default_max_files` (zig:11, `pub`). Gates
/// `collect_vendor_path`'s per-vendor file count (task 6352).
export inline constexpr std::size_t k_default_max_files = 128;

/// @brief The oracle's `default_max_bytes` (zig:12, `pub`): 4 MiB. Gates
/// `collect_vendor_path`'s per-vendor byte budget (task 6352).
export inline constexpr std::size_t k_default_max_bytes = 4 * 1024 * 1024;

/// @brief One discovered source. `jsonl` may contain multiple raw vendor
/// records, one JSON object per line.
export struct raw_source {
  vendor      v;                ///< Which vendor this source came from.
  bool        enabled   = true; ///< Whether the adapter is configured on.
  bool        available = true; ///< Whether the source could be read at all.
  std::string jsonl;            ///< Raw newline-delimited JSON. Empty when unavailable/disabled.
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

// ===========================================================================
// Discovery half (task 6352): collect_preview_from_paths, the fault
// -injection seam, TranscriptConfig, CliLogAdapter.
// ===========================================================================

/// @brief Caps applied while walking transcript directories. Mirrors the
/// oracle's `CollectorLimits` (zig:178).
export struct collector_limits {
  std::size_t max_files = k_default_max_files; ///< Total files across all three transcript vendors.
  std::size_t max_bytes = k_default_max_bytes; ///< Total bytes across all three transcript vendors PLUS the CLI adapter.
  std::size_t max_records =
      k_default_max_records; ///< Total JSONL records across all three transcript vendors PLUS the CLI adapter.
};

/// @brief Per-vendor transcript location configuration. Mirrors the
/// oracle's `TranscriptConfig` (zig:190). Assembled by the `report`
/// handler from `engine.config`'s resolved `[introspection.transcripts]`
/// plus the environment's `$HOME` — see this file's header, "WHY NO
/// `collect_configured_preview`".
export struct transcript_config {
  std::string home_dir;               ///< The operator's home directory, for the three built-in paths.
  bool        claude_enabled = true;  ///< Whether the Claude adapter is configured on.
  std::string claude_path;            ///< Operator override; empty selects `home_dir/.claude/projects`.
  bool        codex_enabled = true;   ///< Whether the Codex adapter is configured on.
  std::string codex_path;             ///< Operator override; empty selects `home_dir/.codex/sessions`.
  bool        copilot_enabled = true; ///< Whether the Copilot adapter is configured on.
  std::string copilot_path;           ///< Operator override; empty selects `home_dir/.copilot/session-state`.
};

/// @brief Which filesystem step `fs_fault` may be asked to fail. Mirrors
/// the oracle's private `FsOperation` (zig:184) — exported here (unlike
/// the oracle's file-private original) because the seam is only useful to
/// a test in a SEPARATE translation unit in this tree.
export enum class fs_operation : std::uint8_t { selected_stat, directory_open, directory_walk, file_stat, file_read };

/// @brief Injectable filesystem-fault seam for tests. `fault(v, op, path)`
/// returning true makes `collect_vendor_path` behave as if that operation
/// had failed, WITHOUT touching the real filesystem. An empty (default
/// -constructed) callable never fails anything — the production path.
/// Mirrors the oracle's private `FsSeam` (zig:185), widened from a
/// context-pointer/fn-pointer pair to a `std::function` closure, the same
/// translation `planar.engine.workbench.root`'s `env_lookup` and this
/// binary's own `context::env_lookup` already use for an injectable
/// environment.
export using fs_fault = std::function<bool(vendor, fs_operation, std::string_view)>;

/// @brief The outcome of one `cli_log_adapter::read` call. Mirrors the
/// oracle's three-state `anyerror!?[]u8`: a plain C++ `std::optional`
/// cannot distinguish "no data, no error" from "the read failed", and the
/// distinction is observable — only the latter raises
/// `warning_kind::cli_adapter_failed`.
export enum class cli_read_status : std::uint8_t {
  ok,          ///< `bytes` holds the read JSONL.
  unavailable, ///< No data (e.g. logging off at the DB layer); no warning.
  failed,      ///< The read itself failed; raises `cli_adapter_failed`.
};

/// @brief One `cli_log_adapter::read` result.
export struct cli_read_result {
  cli_read_status status = cli_read_status::unavailable; ///< Which of the three states this is.
  std::string     bytes;                                 ///< Valid only when `status == ok`.
};

/// @brief Read-only boundary for authoritative CLI rows. The adapter is
/// supplied by the binary that owns the local API; this engine module
/// never opens SQLite. Mirrors the oracle's `CliLogAdapter` (zig:203).
export struct cli_log_adapter {
  bool                                                  enabled = false; ///< Whether the CLI-log source is configured on.
  std::function<cli_read_result(std::size_t max_bytes)> read;            ///< Caller-supplied reader, bounded by `max_bytes`.
};

/// @brief Discover configured/built-in transcript paths and collect a
/// bounded, read-only preview. Overrides accept a file, a directory
/// (recursively), or the documented `/**/*.jsonl` suffix (stripped before
/// resolution). Paths are sorted before normalization so filesystem
/// enumeration order cannot affect results. Mirrors the oracle's
/// `collectPreviewFromPaths`/`collectPreviewFromPathsWithFs` (zig:215-290),
/// collapsed to one function: `fault` defaults to an empty (always-false)
/// seam, the production path the oracle reaches through the two-argument
/// public wrapper.
/// @param config Per-vendor transcript locations.
/// @param cli The CLI-log adapter, or unset to treat it as unavailable
/// (mirrors the oracle's `cli: ?CliLogAdapter = null` arm).
/// @param limits The file/byte/record caps.
/// @param fault The fault-injection seam (tests only; empty in production).
/// @return The aggregated, redacted preview.
export auto collect_preview_from_paths(const transcript_config& config, const std::optional<cli_log_adapter>& cli,
                                       const collector_limits& limits = {}, const fs_fault& fault = {}) -> preview;

} // namespace planar::engine::introspection_adapters
