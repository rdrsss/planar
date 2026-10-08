/// @file introspection_preview.cppm
/// @brief `planar.introspection_preview` — layer-1 redacted-preview
/// vocabulary shared by `engine_introspect` and
/// `engine_introspection_adapters` (decision 981, plan 996 task 6352).
///
/// PROBLEM this module closes: the `report` handler needs
/// `engine_introspect::bundle` to carry a `preview` field populated by
/// `engine_introspection_adapters::collect_preview_from_paths`, the same
/// shape the oracle's `Bundle.preview: ?adapters.Preview` uses. `preview`
/// (plus the four enums it is built from — `vendor`, `category`,
/// `coverage_state`, `warning_kind` — and the three row structs it owns —
/// `warning_row`, `signal_row`, `coverage_row`) was defined inside
/// `engine_introspection_adapters` (task 6102, layer 2). `cmake/
/// architecture.cmake`'s D15 forbids an `engine_* -> engine_*` dependency
/// edge — `engine_introspect -> engine_introspection_adapters` FATALs at
/// configure time with the D15 diagnostic, verified experimentally by a
/// reviewer on task 6121 — so `engine_introspect` cannot simply import
/// `engine_introspection_adapters` to reach the type.
///
/// The reviewer on task 6121 recommended, and decision 981 settled, option
/// (a): extract the shared type to a NEW layer-1 module both engine
/// buckets depend on downward, the same shape `scope_ref` (D19),
/// `json_text`, `adapter`, `http`, `process`, `policy` and `cliapp` already
/// use for exactly this reason. The rejected alternative, (b), would have
/// had the layer-3 `report` handler splice a separately-rendered preview
/// block into `render_json`'s output via string surgery — splitting ONE
/// WIRE FORMAT ACROSS TWO LAYERS, worse than any of the seven precedents.
/// See decision 981's body for the full argument.
///
/// ## WHAT MOVED, AND WHAT DID NOT
///
/// Moved here: `vendor`, `category`, `coverage_state`, `warning_kind`,
/// `warning_row`, `signal_row`, `coverage_row`, `preview` — pure data, no
/// behavior. `raw_source` (collect_preview's INPUT type) and every
/// function (`collect_preview`, `discover`, and this cycle's discovery
/// half) stay in `engine_introspection_adapters`: `raw_source` is specific
/// to that bucket's own collector contract and `engine_introspect` never
/// constructs one, so moving it here would widen this module's surface for
/// no consumer. `k_max_evidence_buckets` and `k_default_max_records` (and
/// this cycle's `k_default_max_files`/`k_default_max_bytes`) also stay —
/// they gate `engine_introspection_adapters`' OWN algorithms
/// (`add_aggregate`'s bucket cap, `extract_claude`'s pending-tool cap,
/// `collect_vendor_path`'s file/byte/record caps) and `engine_introspect`
/// never reads them.
///
/// `engine_introspection_adapters::vendor` etc. (task 6102's original,
/// reviewer-approved names) are kept working via `export using` — the
/// header comment naming them layer-2 was accurate when task 6102 wrote it
/// and is corrected in this cycle's change to that file, per decision
/// 981's own closing paragraph: "this milestone has found eight comments
/// asserting what their code no longer does... adding a ninth ... would be
/// indefensible."
module;

export module planar.introspection_preview;

import std;

namespace planar::introspection_preview {

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
  unsupported_layout,
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
  std::string   verb_path;  ///< The bounded verb path. For `vendor::cli_log` it is
                            ///< the JSONL boundary's value, which the report
                            ///< handler has already masked against the live CLI
                            ///< catalog (`<unrecognized>` for a stored path the
                            ///< catalog rejects); this module does not mask.
  category      cat;        ///< The evidence category.
  std::uint32_t count = 0;  ///< How many raw records collapsed into this bucket.
  std::string   first_seen; ///< Earliest timestamp merged into this bucket.
  std::string   last_seen;  ///< Latest timestamp merged into this bucket.
};

/// @brief One source's scan tally.
export struct coverage_row {
  vendor         v;                                               ///< The vendor.
  coverage_state state                = coverage_state::observed; ///< Overall state.
  std::uint32_t  scanned              = 0;                        ///< Lines examined.
  std::uint32_t  malformed            = 0; ///< Lines that failed to parse or violated the recognized envelope.
  std::uint32_t  normalized           = 0; ///< Lines that produced (or merged into) a signal.
  std::uint32_t  ignored              = 0; ///< Lines that parsed but carried no evidence.
  std::uint32_t  capped               = 0; ///< Lines that would have produced a new bucket past the evidence cap.
  std::uint64_t  bytes_read           = 0; ///< Transcript bytes read for this vendor (a tail read counts only the tail).
  std::uint32_t  files_partial        = 0; ///< Files read from their tail because they alone exceeded the byte budget.
  std::uint32_t  files_skipped_cap    = 0; ///< Files not read because a file, byte or record cap left no room.
  std::uint32_t  files_skipped_window = 0; ///< Files not read because their modification time is before the window.
};

/// @brief Preview output. Owns only normalized keys and timestamps —
/// mirrors the oracle's own comment on `Preview`
/// (`zig/src/engine/introspection_adapters.zig:53`) verbatim, which
/// claims nothing stronger. It does NOT itself redact `cli_log`'s
/// `verb_path`: the value arrives already masked by the report handler's
/// catalog predicate. See that field's doc comment.
export struct preview {
  std::vector<signal_row>   signals;  ///< Aggregated evidence buckets, deduplicated and sorted.
  std::vector<coverage_row> coverage; ///< Per-vendor scan tallies, sorted by vendor.
  std::vector<warning_row>  warnings; ///< Coverage- and cap-driven warnings.
};

} // namespace planar::introspection_preview
