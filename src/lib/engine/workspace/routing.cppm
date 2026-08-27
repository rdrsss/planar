/// @file routing.cppm
/// @brief `planar.engine.workspace.routing` — the routing-table DATA MODEL,
/// its decoder, and the two renderers `workspace routing show` needs
/// (plan 996, task 6110).
///
/// Behavior-preserving port (D2) of the READ half of
/// zig/src/engine/workspace/routing.zig (`read`, `fromJsonValue`,
/// `parseProject`, `parseCrossRepo`, `parsePlanarFocus`, `parseLanguages`,
/// `parseStringArray`, `parseIntArray`, `getString`, `getInteger`) together
/// with the rendering half of
/// zig/src/cmd/planar/handlers/workspace/routing/show.zig.
///
/// ## Why the READ half ships without the BUILDER, against the prior note
///
/// The previous cycle recorded in this bucket's CMakeLists that `routing
/// show` should be deferred WITH `routing build`, on the reasoning that
/// "its TEXT arm renders every field of the routing table, so the whole
/// data model has to exist first" and that porting half a leaf is worse
/// than deferring both.
///
/// The first clause is true and the conclusion does not follow. The data
/// model does have to exist first — but it is produced by DECODING
/// `routing-table.json`, not by building one. `show` never calls the
/// builder: it reads the file off disk (see show.zig:24) and, in its text
/// arm, hands the bytes to `routing.read`. A decoder plus a renderer is a
/// COMPLETE leaf, testable end to end against a fixture file, with no
/// dependency on the 1410-line builder whatsoever.
///
/// So this is not half a leaf. It is one whole leaf whose sibling stays
/// deferred, and the distinction is worth stating because the earlier note
/// would otherwise read as a standing decision not to do this.
///
/// The consequence to be honest about: until `routing build` lands, the
/// C++ binary can only SHOW a table some other binary wrote. That is
/// correct behavior for this leaf — the file is the interface — but it does
/// mean `build` then `show` is not yet a round trip in this tree.
///
/// ## The `--json` arm never parses, and that is the whole arm
///
/// `show --json` is a VERBATIM cat of the file's bytes. It does not decode,
/// validate, or re-encode, so it happily emits a file that is not JSON at
/// all. Oracle-captured, with the file containing the sixteen bytes `this
/// is not json`:
///
///     $ planar workspace routing show --json
///     this is not json
///     exit 0
///
/// The only transformation is the terminator: a newline is appended when —
/// and only when — the last byte is not already one. An EMPTY file
/// therefore yields a lone `\n`, also captured. `show_json` below is that
/// rule and nothing else.
///
/// This is why the two arms have completely different failure surfaces, and
/// why `decode` is reached only from the text arm.
///
/// ## FOUR decode failures, one message template, two exit codes
///
/// The Zig handler interpolates `@errorName(e)` into a single message, so
/// the distinctions are invisible in the format string and have to be
/// captured one input at a time. All four were, against a pinned scratch
/// arena:
///
///     input                          tag                     exit
///     -----------------------------  ----------------------  ----
///     this is not json               SyntaxError                1
///     x{}   /   {} trailing   /
///       {"a":1,}   /   {'a':1}   /
///       {"a":NaN}   /   a raw
///       control byte in a string     SyntaxError                1
///     an EMPTY file   /   spaces
///       only   /   {   /   {"a":
///       /   {"a":"b                  UnexpectedEndOfInput       1
///     a duplicate object key         DuplicateField             1
///     []   /   42   /   "hi"   /
///       null   /   true   /   a
///       missing required field       InvalidInput               2
///
/// The first three tags all exit 1 and differ only in stderr text; the
/// fourth also moves the EXIT CODE. A port that collapsed them would keep
/// every stderr payload plausible while silently changing one exit code and
/// three messages, and no assertion on the message template alone would
/// catch any of it.
///
/// `decode_error` therefore keeps all four apart, and `decode_error_name`
/// supplies the exact interpolated tag. The three exit-1 arms come straight
/// from `planar.json_dom`'s `json_parse_reason`, which exists for this
/// caller — see its own documentation for why the classification lives
/// there rather than here.
///
/// Note the last row: a top-level `[]` / `42` / `"hi"` / `null` / `true`
/// PARSES fine and then fails the object check, so it is `InvalidInput` at
/// exit 2, not a syntax error. Captured for all five.
///
/// ## Which fields are required, which default, and which are merely dropped
///
/// Three distinct behaviours, and they do not follow from field type:
///
///   - REQUIRED, absent or wrong-typed => `invalid`: `schema_version`,
///     `workspace_id`, `workspace_slug`, `workspace_name`, `generated_at`,
///     the top-level `projects` array, the top-level `cross_repo` object,
///     `cross_repo.dependency_edges`, and — per project —
///     `planar_focus`. A project with no `planar_focus` key fails the whole
///     decode; oracle-captured at exit 2.
///   - DEFAULTED: `generator_version` falls back to `static-v1` when absent
///     or non-string. Captured: a table with no such key renders
///     `generated: GEN (static-v1)`.
///   - DROPPED SILENTLY: every per-project string field defaults to empty;
///     non-string elements inside a string array, non-integer elements
///     inside an integer array, and non-numeric `languages` values are
///     skipped INDIVIDUALLY rather than failing the array; a
///     `dependency_edges` element that is not an object is skipped.
///
/// ## `languages` is insertion-ordered, and the oracle's order is a hash artifact
///
/// The field decodes into a `vector<pair<string,double>>` rather than a map
/// because `std.json.ArrayHashMap` preserves insertion order and this
/// decoder must round-trip whatever order the file carries.
///
/// Worth recording for whoever ports the BUILDER: on the write side that
/// order is not meaningful. `countLanguages` accumulates into a
/// `std.StringHashMap` and emits during iteration, so the key order in a
/// freshly built table is Zig's hash order. A three-language repo probed
/// here emitted `javascript, markdown, json` — neither sorted nor
/// frequency-ordered. A C++ builder cannot and should not reproduce it; the
/// order is unspecified output, and any parity assertion over a BUILT
/// table's `languages` key order would be pinning a hash seed.
///
/// `show`'s text renderer never prints `languages`, so nothing here is
/// affected by it.
///
/// ## The text renderer's four conditional shapes
///
/// All four oracle-captured, because none is guessable from the struct:
///
///   - `capabilities:` prints `(none)` when empty, else the tags joined
///     with `", "`.
///   - `summary:` prints `(no summary)` when the string is EMPTY — note
///     this is keyed on the summary itself, not on `summary_source`.
///   - `depends_on:` is OMITTED ENTIRELY when empty. It is the only
///     per-project line that can be absent; `capabilities` and `summary`
///     always print.
///   - The trailing `cross-repo edges:` block is omitted entirely when
///     there are no edges. When present it is preceded by a blank line.
///
/// The header always prints, so a zero-project table still emits three
/// lines and a trailing blank one.
module;

export module planar.engine.workspace.routing;

import std;
import planar.json_dom;

namespace planar::engine::workspace::routing {

/// @brief The `schema_version` a table built by this generation carries.
///
/// Unused by the decoder — which accepts any integer — and exported for the
/// builder and for tests that construct fixtures.
export inline constexpr std::int64_t schema_version_current = 1;

/// @brief The `generator_version` the static (non-enriched) builder stamps,
/// and the value the decoder substitutes when the field is absent.
export inline constexpr std::string_view generator_version_static = "static-v1";

/// @brief One `from -> to` cross-repo dependency, with its provenance.
export struct dependency_edge {
  std::string from;   ///< The depending project's slug.
  std::string to;     ///< The depended-on project's slug.
  std::string reason; ///< Prose provenance, e.g. `go.mod replace`.
};

/// @brief Workspace-wide state that belongs to no single project.
export struct cross_repo {
  std::vector<std::int64_t>    plans_scoped_to_org;     ///< Plan ids scoped to the org association.
  std::vector<std::int64_t>    questions_scoped_to_org; ///< Open question ids scoped to the org.
  std::vector<dependency_edge> dependency_edges;        ///< Every project's deps, flattened.
};

/// @brief One project's Planar-side workload summary.
export struct planar_focus {
  std::vector<std::int64_t> active_plans;       ///< Active plan ids touching this project.
  std::int64_t              open_tasks     = 0; ///< Count of todo/doing/blocked tasks.
  std::int64_t              open_questions = 0; ///< Count of open questions.
  std::vector<std::int64_t> recent_session_ids; ///< Always empty in a built table; decoded anyway.
};

/// @brief One member repository's routing entry.
export struct project_route {
  std::string slug;           ///< The `projects.slug`.
  std::string root_path;      ///< Absolute path to the checkout.
  std::string git_remote;     ///< Remote URL, or empty.
  std::string summary;        ///< One-line description, or empty.
  std::string summary_source; ///< `readme`, `manual`, or empty.

  std::vector<std::string> capabilities;        ///< Sorted, deduped capability tags.
  std::string              capabilities_source; ///< `static`, `manual`, or empty.
  std::vector<std::string> depends_on;          ///< Sibling slugs this project depends on.
  std::string              depends_on_source;   ///< `go.mod`, `package.json`, `manual`, or empty.
  std::vector<std::string> entry_points;        ///< Up to five notable paths.

  /// @brief Language -> share of files, in the file's own key order.
  ///
  /// A vector, not a map: insertion order is round-tripped. See this file's
  /// header for why the order in a freshly BUILT table is a hash artifact
  /// and must not be asserted on.
  std::vector<std::pair<std::string, double>> languages;

  planar_focus focus; ///< The `planar_focus` object. REQUIRED in the file.
};

/// @brief A decoded `routing-table.json`.
export struct routing_table {
  std::int64_t schema_version = 0; ///< Table format version.
  std::int64_t workspace_id   = 0; ///< The org association's id.
  std::string  workspace_slug;     ///< The org's slug.
  std::string  workspace_name;     ///< The org's display name.
  std::string  generated_at;       ///< Build timestamp, from SQLite's `strftime`.
  std::string  generator_version;  ///< Generator tag; defaults to `static-v1`.

  std::vector<project_route> projects; ///< Member repositories, in build order.
  cross_repo                 cross;    ///< Workspace-wide state.
};

/// @brief Why a decode failed.
///
/// The two arms exist to preserve two DIFFERENT process exit codes — see
/// this file's header. Do not collapse them.
export enum class decode_error : std::uint8_t {
  syntax,          ///< An unexpected byte. Oracle tag `SyntaxError`, exit 1.
  end_of_input,    ///< The document ended mid-value. Oracle tag `UnexpectedEndOfInput`, exit 1.
  duplicate_field, ///< An object repeated a key. Oracle tag `DuplicateField`, exit 1.
  invalid,         ///< Well-formed JSON that is not a usable table. Oracle tag `InvalidInput`, exit 2.
};

/// @brief The error tag the oracle interpolates into its failure message.
///
/// `decoding routing table failed: <tag>`.
/// @param value The decode failure.
/// @return `"SyntaxError"` or `"InvalidInput"`.
export auto decode_error_name(decode_error value) -> std::string_view;

/// @brief Decode `routing-table.json` bytes into the table.
///
/// See this file's header for the required / defaulted / silently-dropped
/// split — it is not derivable from the struct.
/// @param raw The complete file contents.
/// @return The table, or why it could not be decoded.
export auto decode(std::string_view raw) -> std::expected<routing_table, decode_error>;

/// @brief Decode from an already-parsed DOM value.
///
/// The half of `decode` below the JSON parse, split out so a test can feed
/// a hand-built DOM without round-tripping through text.
/// @param value The parsed document; must be an object.
/// @return The table, or nullopt when a required field is absent or
/// wrong-typed.
export auto from_json(const json_dom::json_value& value) -> std::optional<routing_table>;

/// @brief Render the `--json` arm: the file's bytes, verbatim.
///
/// Appends a newline only when the last byte is not already one. Performs
/// NO parsing — non-JSON input passes straight through. See this file's
/// header.
/// @param raw The complete file contents.
/// @return The complete stdout payload, terminator included.
export auto show_json(std::string_view raw) -> std::string;

/// @brief Render the human-readable arm.
/// @param table The decoded table.
/// @return The complete stdout payload, terminator included.
export auto render_text(const routing_table& table) -> std::string;

/// @brief The refusal emitted when `routing-table.json` does not exist.
///
/// Note `workspace regenerate`'s equivalent message OMITS the path. The two
/// are NOT the same string — do not factor them together.
/// @param path The absolute path that was looked for.
/// @return The message BODY, without an `error: ` prefix or terminator.
export auto missing_table_error(std::string_view path) -> std::string;

} // namespace planar::engine::workspace::routing
