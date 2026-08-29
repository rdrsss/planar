/// @file routing.cppm
/// @brief `planar.engine.workspace.routing` — the routing-table DATA MODEL,
/// its decoder, its BUILDER, and the two renderers `workspace routing show`
/// needs (plan 996, tasks 6110 and 6275).
///
/// Behavior-preserving port (D2) of zig/src/engine/workspace/routing.zig,
/// now in full: the READ half (`read`, `fromJsonValue`, `parseProject`,
/// `parseCrossRepo`, `parsePlanarFocus`, `parseLanguages`,
/// `parseStringArray`, `parseIntArray`, `getString`, `getInteger`) landed at
/// task 6110 together with the rendering half of
/// zig/src/cmd/planar/handlers/workspace/routing/show.zig; the WRITE half
/// (`buildWithRules`, `buildProject`, `buildCrossRepo`, `detectCapabilities`,
/// `inferDependencies`, `detectEntryPoints`, `countLanguages`,
/// `loadPlanarFocus`, `firstParagraph*`, `defaultCapabilityRules`,
/// `loadCapabilityRules`, `loadOverrides`, `applyOverrides`, `write`) landed
/// at task 6275.
///
/// ## `build` then `show` is a round trip in this tree as of task 6275
///
/// Task 6110 shipped the READ half alone, against a standing note that
/// `routing show` should be deferred WITH `routing build`. That note
/// reasoned the text arm "renders every field of the routing table, so the
/// whole data model has to exist first" — true, and the conclusion did not
/// follow, because the model is produced by DECODING `routing-table.json`
/// rather than by building one. `show` never calls the builder.
///
/// The honest consequence recorded at the time was that the C++ binary could
/// only SHOW a table some other binary wrote. That is no longer so: the
/// builder below writes the file the decoder reads, and
/// `routing_build.t.cpp` pins the loop end to end — build a table from a
/// scratch database, decode the bytes back, and compare field by field.
///
/// ## THREE deliberate divergences from the oracle, all measured
///
/// Each is reproduced-as-observed or knowingly diverged-from, never guessed,
/// and each is pinned by a test so it cannot drift silently.
///
///   1. `languages` KEY ORDER — divergence. See the section below.
///   2. A capability pattern CONTAINING A SLASH never matches in the oracle,
///      because `patternFinds` frees the joined directory path before it
///      opens it. Reproduced as observed. See the section below.
///   3. `routing show`'s AMBIGUOUS refusal exits 2, not 1. That is a
///      CORRECTION to task 6110's port rather than a divergence — see
///      handlers/workspace.cppm.
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
/// ## DIVERGENCE 1: `languages` key order. The oracle's is a hash artifact;
/// this builder sorts.
///
/// The field decodes into a `vector<pair<string,double>>` rather than a map
/// because `std.json.ArrayHashMap` preserves insertion order and the DECODER
/// must round-trip whatever order the file carries. That much is unchanged.
///
/// On the WRITE side the oracle's order is not meaningful and cannot be
/// reproduced. `countLanguages` accumulates into a `std.StringHashMap` and
/// emits during iteration, so a freshly built table carries Zig's hash order.
/// Captured against the oracle in a pinned scratch arena, one build, three
/// repositories:
///
///     alpha (2 keys)   go, markdown
///     beta  (3 keys)   typescript, markdown, json
///     gamma (4 keys)   javascript, markdown, python, json
///
/// Neither sorted, nor frequency-ordered, nor insertion-ordered by the walk
/// (the walk visits `beta`'s `README.md` before `src/index.ts`). It is a
/// hash seed, it is not stable across Zig versions, and a differential that
/// compared two BUILT tables byte-for-byte would report a divergence on this
/// field that is not one.
///
/// So this builder emits `languages` SORTED BY KEY, ascending, byte-wise.
/// That is a deliberate divergence, it is the only field where the two
/// implementations may legitimately disagree, and `routing_build.t.cpp` pins
/// the SORTED order rather than any captured sequence. Do not "restore
/// parity" here: there is no parity to restore.
///
/// The SHARES themselves are byte-identical and are asserted as such — only
/// the key sequence differs.
///
/// Every other collection this builder emits is explicitly ordered by the
/// oracle itself and is NOT a divergence. Checked one at a time rather than
/// assumed from `languages`:
///
///   - `capabilities` — accumulated in RULE order, then sorted and deduped.
///   - `depends_on` — accumulated into a `StringHashMap`, then SORTED.
///   - `entry_points` — accumulated into a `StringHashMap`, then SORTED,
///     then truncated to five. The truncation is after the sort, so it is
///     deterministic.
///   - `dependency_edges` — sorted by `(from, to)`.
///   - `plans_scoped_to_org` / `questions_scoped_to_org` / `active_plans` —
///     `order by id` in SQL.
///
/// `show`'s text renderer never prints `languages`, so the `show` leaf is
/// unaffected either way.
///
/// ## DIVERGENCE 2: a capability pattern containing `/` never matches
///
/// `patternFinds` splits a pattern such as `proto/*.proto` on its slash,
/// JOINS the left half onto the project root, and scans that directory. It
/// also frees the joined path immediately:
///
///     if (std.mem.indexOfScalar(u8, pattern, '/')) |idx| {
///         const subdir = std.fs.path.join(...) catch return false;
///         defer allocator.free(subdir);      // <-- ends with the IF BLOCK
///         dir = subdir;
///         glob = pattern[idx + 1 ..];
///     }
///     var opened = std.Io.Dir.cwd().openDir(fsIo(), dir, ...) catch return false;
///
/// Zig's `defer` runs at the end of the enclosing BLOCK, which here is the
/// `if` payload — so `subdir` is freed before `openDir` ever sees it and
/// `dir` dangles. The `catch return false` then swallows the failure.
///
/// Measured, not inferred. Five consecutive oracle builds over three
/// fixtures, identical every time:
///
///     rootproto   a.proto at the repo ROOT      -> ["protobuf"]
///     subproto    proto/a.proto only            -> []
///     subdirrule  schemas/x.frob + a custom
///                 rule match_any=["schemas/*.frob"] -> []
///
/// So the default `protobuf` rule fires only through its NO-SLASH arm
/// (`*.proto`), and the `proto/*.proto` arm beside it is dead. An operator
/// rules file using any slash-bearing pattern silently matches nothing.
///
/// This port reproduces the OBSERVED behavior — `pattern_finds` returns
/// false for any pattern containing a slash — rather than the intended one,
/// on the same D2 grounds that keep `--enrich` a no-op and `tree --sort`
/// inert. Implementing the INTENT would make this binary emit a capability
/// tag the oracle never emits, which is a parity break dressed as a bug fix.
///
/// It is a real oracle defect and belongs in a task row against
/// zig/src/engine/workspace/routing.zig, not in a silent repair here. When
/// that fix lands, `pattern_finds` gains the join-and-scan arm and the test
/// pinning the empty result flips WITH it.
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
import planar.db;
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

// ===========================================================================
// THE BUILDER (task 6275)
// ===========================================================================

/// @brief One capability-detection rule.
///
/// A rule fires when EVERY `match_all` pattern finds a directory entry AND —
/// when `match_any` is non-empty — at least one `match_any` pattern does,
/// AND the `package_dep` and `go_main` post-conditions hold. A rule with
/// BOTH pattern lists empty never fires, which is what makes a `[[rule]]`
/// header with only a `tag` inert rather than universal.
///
/// Patterns are matched against the entries of ONE directory, not walked
/// recursively. See this file's header for why a pattern containing a slash
/// never matches at all.
export struct capability_rule {
  std::string              tag;             ///< The capability emitted on a match.
  std::vector<std::string> match_all;       ///< Every one of these must find an entry.
  std::vector<std::string> match_any;       ///< At least one must, when non-empty.
  std::string              package_dep;     ///< When set, `package.json` must declare it.
  bool                     go_main = false; ///< When set, a `package main` must exist.
};

/// @brief The seven built-in rules, in the order they are evaluated.
///
/// Order is observable only through `go-service` suppressing `go-library`
/// (see `detect_capabilities`); the emitted list is sorted afterwards.
/// @return The default rule set.
export auto default_capability_rules() -> std::vector<capability_rule>;

/// @brief Why a capability-rules file could not be loaded.
///
/// TWO arms because the oracle answers with TWO EXIT CODES behind one
/// message template, exactly as `decode_error` does. Oracle-captured:
///
///     match_all = [oops]   -> exit 1  loading capability rules failed: ParseFailed
///     tag = unquoted       -> exit 2  loading capability rules failed: InvalidInput
///
/// Do not collapse them.
export enum class rules_error : std::uint8_t {
  parse_failed, ///< The TOML value did not parse. Oracle tag `ParseFailed`, exit 1.
  invalid,      ///< A scalar was not a quoted string. Oracle tag `InvalidInput`, exit 2.
};

/// @brief The error tag the oracle interpolates into its failure message.
/// @param value The load failure.
/// @return `"ParseFailed"` or `"InvalidInput"`.
export auto rules_error_name(rules_error value) -> std::string_view;

/// @brief Load operator capability rules from a TOML file.
///
/// The file REPLACES the defaults wholesale rather than extending them —
/// oracle-captured: a file declaring one `docs-only` rule produced exactly
/// `["docs-only"]` and no built-in tag. The caller falls back to
/// default_capability_rules() only when the result is EMPTY, which is also
/// what an ABSENT file yields.
///
/// The parser is line-oriented and deliberately narrow, mirroring the
/// original: `#` starts a comment ANYWHERE on the line (so no pattern may
/// contain one), `[[rule]]` opens a rule, and only the five keys of
/// `capability_rule` are recognised. Anything else on a line is ignored.
/// Keys are matched by PREFIX, so `tag_of = "x"` sets `tag`.
/// @param path The rules file; absence is not an error.
/// @return The rules, or why the file could not be read.
export auto load_capability_rules(const std::filesystem::path& path) -> std::expected<std::vector<capability_rule>, rules_error>;

/// @brief An operator's replacements for one project's derived fields.
///
/// Each member is separately optional: an ABSENT key leaves the derived
/// value alone, while a PRESENT one replaces it and flips the matching
/// `*_source` to `manual`. Note the third state — a present but EMPTY array
/// still counts as present, so `{"capabilities": []}` clears the tags and
/// still sets `capabilities_source` to `manual`. Oracle-captured.
export struct project_override {
  std::optional<std::string>              summary;      ///< Replaces `summary`.
  std::optional<std::vector<std::string>> capabilities; ///< Replaces `capabilities`, sorted and deduped.
  std::optional<std::vector<std::string>> depends_on;   ///< Replaces `depends_on`, sorted and deduped.
};

/// @brief The parsed `routing-table-overrides.json`.
///
/// ## Why this type is NOT called `overrides`, which it obviously should be
///
/// Doxygen 1.18.0 tokenizes the `override` KEYWORD out of the middle of an
/// identifier when that identifier appears as a template argument in a
/// trailing return type. Any name containing the substring `override`
/// therefore fails the `make cpp-lint` gate here.
///
/// The failure does not name the real cause, which is why this is worth
/// recording. With `-> std::expected<overrides, decode_error>` the gate
/// reports:
///
///     error: Member decode_error (variable) of namespace
///     planar::engine::workspace::routing is not documented
///
/// — doxygen has read the SECOND template argument as a variable declaration
/// after failing on the first. Fixing that one site only moves the error to
/// the DEFINITION in routing_build.cpp, where the function's first `if`
/// becomes an undocumented namespace member.
///
/// Three candidate explanations were tested; the first two are WRONG and are
/// named so nobody re-tests them:
///
///   - "`decode_error` cannot be documented" — no. `decode` above returns
///     `std::expected<routing_table, decode_error>` unqualified and parses.
///   - "the plural `overrides` collides with the verb" — no. Renaming to
///     `override_set` and then to `manual_overrides` changed nothing.
///   - THE ACTUAL CAUSE, and it only became visible once BOTH template
///     arguments were qualified, which made doxygen print the identifier it
///     had built:
///
///         Member load_overrides(...) -> std::expected< routing::manual_
///         override s (function) ... is not documented
///
///     `manual_overrides` came back as `manual_` + `override` + `s`.
///
/// So the fix is the NAME, not a qualifier: `manual_edits` contains no
/// `override` substring and parses clean with no workaround anywhere. The
/// two FUNCTIONS below keep their `_overrides` names — doxygen handles the
/// declarator fine and only mangles template arguments — so the vocabulary
/// the operator sees (`routing-table-overrides.json`) is unchanged.
export struct manual_edits {
  std::int64_t schema_version = 0; ///< Read but never enforced, in the oracle or here.
  /// @brief Per-project entries, keyed by project slug.
  ///
  /// A vector rather than a map because it is only ever looked up by slug
  /// and the file's order is not observable; an entry naming a project that
  /// is not a workspace member is silently ignored.
  std::vector<std::pair<std::string, project_override>> projects;
};

/// @brief Load `routing-table-overrides.json`.
///
/// Shares `decode_error` with the table decoder because the oracle's failure
/// surface is the same one: three parse tags at exit 1 and `InvalidInput` at
/// exit 2 for a document that parsed but is not an object. Oracle-captured:
///
///     `not json`      -> exit 1  loading routing overrides failed: SyntaxError
///     a bare `[]`     -> exit 2  loading routing overrides failed: InvalidInput
///
/// An ABSENT file yields empty overrides and is not an error.
/// @param path The overrides file.
/// @return The overrides, or why they could not be decoded.
export auto load_overrides(const std::filesystem::path& path) -> std::expected<manual_edits, decode_error>;

/// @brief Apply operator overrides to a built table, in place.
///
/// Only projects present in BOTH the table and the overrides are touched.
/// @param table The table to modify.
/// @param values The loaded overrides.
export auto apply_overrides(routing_table& table, const manual_edits& values) -> void;

/// @brief Why a table could not be built.
export enum class build_error : std::uint8_t {
  query_failed, ///< The database refused. Oracle tag `QueryFailed`.
};

/// @brief Build a routing table for one org from workspace membership.
///
/// Reads `projects` joined to `project_associations`, then for each member
/// reads its checkout off disk: README first paragraph, capability rules,
/// `go.mod` / `package.json` sibling dependencies, entry points and a
/// language census. A member whose `root_path` does not exist yields a
/// fully EMPTY project entry rather than an error — oracle-captured against
/// a registered-then-deleted directory.
///
/// `generated_at` comes from SQLite's own
/// `strftime('%Y-%m-%dT%H:%M:%SZ','now')`, NOT from the host clock, so the
/// stamp matches every other timestamp the database issues.
///
/// The oracle re-resolves the org by id inside its builder; this port takes
/// the already-resolved slug and name from the caller, which resolved them a
/// moment earlier to select the workspace at all. The re-read is
/// unobservable — the same row, in the same process, with no write between.
/// @param conn An open connection.
/// @param org_id The org association's id.
/// @param org_slug The org's slug, for `workspace_slug`.
/// @param org_name The org's name, for `workspace_name`.
/// @param rules The capability rules to apply.
/// @return The built table, or why it could not be built.
export auto build(db::connection& conn, std::int64_t org_id, std::string_view org_slug, std::string_view org_name,
                  const std::vector<capability_rule>& rules) -> std::expected<routing_table, build_error>;

/// @brief Serialise a table to the exact bytes the oracle writes.
///
/// The layout is `std.json.Stringify` at `.whitespace = .indent_2`, which is
/// a byte contract rather than a formatting preference. It is NOT
/// re-implemented here: this builds a `json_dom::json_value` in the Zig
/// struct's field order and hands it to `json_dom::stringify_indent2`, which
/// already owns those rules and is tested against them. A second
/// indentation engine in this bucket could only drift from the first.
///
/// Shares reach the oracle's bytes through that encoder's `std::to_chars`
/// shortest-round-trip path. Every share is `round(pct * 100) / 100`, and
/// the whole value space this field can hold was captured and agrees: `1`,
/// `0`, `0.05`, `0.2`, `0.33`, `0.4`, `0.67`, `0.95`. Note in particular
/// that a whole share prints as `1`, NOT `1.0` and NOT `1e0`, and that a
/// language rounding below half a percent prints a literal `0` and is still
/// emitted.
///
/// The result carries NO trailing newline; write_table() appends it.
/// @param table The table to serialise.
/// @return The JSON document, without a terminator.
export auto encode(const routing_table& table) -> std::string;

/// @brief Write a table to `path` atomically.
///
/// Serialises through encode(), appends the single trailing newline, writes
/// `<path>.tmp` and renames it into place — so an interrupted build cannot
/// leave a truncated routing table where a working one was.
/// @param path The destination.
/// @param table The table to write.
/// @return True on success.
export auto write_table(const std::filesystem::path& path, const routing_table& table) -> bool;

} // namespace planar::engine::workspace::routing
