/// @file schema.cppm
/// @brief `planar.cliapp.schema` — emits the deterministic flat JSON
/// catalog `zig/tools/cli_usage_lint` consumes (`<bin> schema`), walking a
/// built `CLI::App` (plan 996, task 6123).
///
/// ## The catalog survived the CLI11 swap; here is the measurement
///
/// Decision 948 recorded a RISK — "CLI11 must expose enough structure to
/// rebuild the schema catalog (every command, subcommand, flag, with the
/// `deprecated` and `doc` fields task 6065 requires). If it does not, the
/// catalog emitter needs to keep its own tree representation" — and the
/// task brief carried it forward as an open verdict. It is closed here,
/// against the consumer rather than against the emitter:
///
/// `zig/tools/cli_usage_lint.zig` declares the ENTIRE subset of this
/// document it reads, as three structs:
///
///     SchemaJson  { commands: []CommandJson }
///     CommandJson { command, subcommands, flags }
///     FlagJson    { long, aliases, short }
///
/// Six keys. `deprecated` and the twelve-key `doc` blob are not among
/// them — the lint resolves a command path, collects its allowed flag
/// tokens, and reports an authored `--flag` the binary does not expose.
/// Nothing else in the document reaches it.
///
/// And the `deprecated`/`docs` gap was never real on the C++ side to begin
/// with: the emitter this one replaces (`planar.cli.schema`) had NO
/// `deprecated` field and NO per-node `doc` field on its own `cmd` type
/// either — its file header says so, and it emitted both as hardcoded
/// constants (`"deprecated":null` and the Zig `Doc{}` empty-default
/// shape). Those constants are reproduced verbatim below, so this emitter
/// carries exactly as much information as its predecessor did. The swap
/// costs the catalog nothing.
///
/// VERDICT: `cli_usage_lint` runs UNMODIFIED against this emitter's
/// output. `schema.t.cpp`'s `[lint-parity]` case is the standing proof —
/// it builds the real, unmodified lint tool from
/// `zig/tools/cli_usage_lint.zig` and runs it against a stub binary that
/// answers `schema` from a `CLI::App` built here.
///
/// ## What this emitter DOES lose relative to `planar.cli.schema`
///
/// Named rather than hidden, because the operator asked for the accounting
/// and none of the four is consumed by the lint:
///
///   1. `"summary"` vs `"description"`. `cli::cmd` carried `desc` (the
///      one-line summary) and `long_desc` (the multi-line prose lead-in)
///      as separate fields; `CLI::App` carries ONE description string, so
///      the single-argument overload below emits the same string for both
///      keys.
///
///      NARROWED by task 6065, with a measurement. The claim above that
///      "the Zig oracle's catalog cannot distinguish the two either" is
///      WRONG, and the fix is not free: across the three oracle catalogs
///      69 nodes emit a `summary` that differs from their `description`
///      (57 on `planar`, 12 on `planar-watch`, 0 on `planar-agent`), and
///      it is NOT a parent-only effect — 40 of the 69 are leaves. So the
///      two-argument overload takes the summaries as DATA, keyed by full
///      command path, and each binary passes its generated table. A node
///      absent from the table still falls back to its description, which
///      is what keeps a hand-declared node that predates the table honest
///      rather than blank.
///
///      Still lost: `CLI::App` remains a one-string type, so the summary
///      cannot be read back OFF the tree — only supplied alongside it.
///   2. `"kind"` and `"choices"` are DERIVED from `CLI::Option::
///      get_type_name()` rather than declared. CLI11 populates that string
///      from the option's validators (`:{a,b,c}` for `CLI::IsMember`), so
///      a choice set survives; a flag with no validator and a value is
///      reported as `string`, where the old tree could have declared it
///      `path` or `duration`. No binary's tree in this repo declares
///      either kind, so nothing observable moves today.
///   3. `"list"` / `"count"` are derived from the option's expected-count
///      range instead of two declared booleans.
///   4. `"env"` is always `null`. `CLI11::Option` has `get_envname()`, but
///      no tree in this repo declares an env fallback, so wiring it would
///      be untested code.
module;

export module planar.cliapp.schema;

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cliapp.walk;

namespace planar::cliapp {

/// @brief Emit the flat JSON schema catalog for the tree rooted at `root`.
///
/// ## The one-arg/two-arg split is a per-binary property, not drift
///
/// `planar-agent` and `planar-ext` call THIS overload; `planar` and
/// `planar-watch` call the two-argument one below. That is an intentional
/// split on measured divergence, not an inconsistency to close:
/// `surface_summaries()` on the two-arg side exists because a real gap is
/// being closed — 57 `planar` nodes and 12 `planar-watch` nodes carry a
/// `summary` that differs from their `description` (this module's header,
/// divergence 1, task 6065). `planar-agent` measures ZERO such
/// divergences and `planar-ext` has no oracle catalog to diverge from at
/// all, so a summaries table on either binary would supply data that
/// changes nothing.
///
/// This was not always the answer: the deleted
/// `planar-agent/surface.cppm` used to supply a summaries table ANYWAY,
/// on a uniformity rationale — "the emitter is driven the same way in all
/// three binaries rather than only where it currently changes bytes."
/// Task 6614 (M11.2) deleted that table and reversed the rationale in
/// favor of the one above. Recorded here, not only in that commit
/// message, because a reader who finds `planar-agent`'s one-arg call site
/// alone would otherwise have no way to tell "nothing to diverge from"
/// apart from "not gotten to yet" (task 6628).
/// @param root The command tree root (its own `get_name()` becomes the
/// JSON `"root"` value and the first path segment of every `"command"`
/// string).
/// @return The catalog as a single-line JSON document (no trailing
/// newline — the `schema` verb appends one at the write site).
export auto schema_json(const CLI::App& root) -> std::string;

/// @brief Emit the catalog with a per-command one-line `"summary"` supplied
/// out of band.
///
/// `CLI::App` holds one description string, so a node's short summary has
/// nowhere to live ON the tree. This overload takes it as data instead. See
/// this module's header, divergence 1, for the measurement that made it
/// worth having.
/// @param root The command tree root.
/// @param summaries `(full command path, summary)` pairs — e.g.
/// `{"planar plan next", "Bucketed claim-aware view..."}`. Order is
/// irrelevant; a path that appears more than once resolves to the first
/// match. A command absent from the span falls back to its description.
/// @return The catalog as a single-line JSON document (no trailing
/// newline).
export auto schema_json(const CLI::App& root, std::span<std::pair<std::string_view, std::string_view> const> summaries)
    -> std::string;

/// @brief Emit the catalog with both the out-of-band summaries and the set
/// of flags whose declared default is the EMPTY STRING (plan 996, task
/// 6130).
///
/// ## Why this needs a side-table at all
///
/// `CLI::Option` exposes exactly one accessor for a default,
/// `get_default_str()`, and it returns `""` for BOTH "this flag has no
/// default" and "this flag's default is the empty string". The two are
/// genuinely indistinguishable from a built tree, so `default_literal`
/// mapped every empty one to `null` — correct for the ~500 flags with no
/// default, wrong for the four the oracle declares with `""`:
///
///     planar workbench edit --editor
///     planar workflow run   --args
///     planar workflow run   --worktree
///     planar workflow run   --sandbox-root
///
/// Eight bytes of a 353,934-byte catalog, and the ONLY eight in which
/// `planar`'s catalog differed from the oracle's.
///
/// Three in-tree encodings were considered and rejected before this:
/// a sentinel default string (`harvest` reads `get_default_str()` and
/// would have seeded the sentinel into every handler's `parsed_args`), a
/// marker `CLI::Validator` (`Option::get_validator` is non-const and
/// THROWS when absent, so a const emitter cannot ask), and a marker
/// `type_name` (visible in `--help`, and read back by `kind_of` and
/// `choices_of`). Supplying it as data is what this module already does
/// for the one-line summary, for the same underlying reason.
/// @param root The command tree root.
/// @param summaries `(full command path, summary)` pairs; see the
/// two-argument overload.
/// @param empty_string_defaults `(full command path, flag long name)`
/// pairs — e.g. `{"planar workflow run", "--args"}`. A flag named here
/// reports `"default":""`; one absent keeps `null`. Naming a flag that
/// does not exist is inert.
/// @return The catalog as a single-line JSON document (no trailing
/// newline).
export auto schema_json(const CLI::App& root, std::span<std::pair<std::string_view, std::string_view> const> summaries,
                        std::span<std::pair<std::string_view, std::string_view> const> empty_string_defaults) -> std::string;

/// @brief One exit code a binary documents, with the meaning every command
/// that lists it shares.
export struct exit_code_doc {
  int              code = 0; ///< The process exit status.
  std::string_view meaning;  ///< One sentence saying what the status means.
};

/// @brief A meaning that replaces the binary-wide one for a single command.
export struct exit_meaning_override {
  std::string_view command;  ///< Full command path, e.g. `planar plan closeout`.
  int              code = 0; ///< The status whose meaning differs on this command.
  std::string_view meaning;  ///< The meaning on this command.
};

/// @brief The per-command examples and exit codes of one binary, supplied as
/// data beside its summary table.
///
/// `CLI::App` has no slot for either, so they travel out of band exactly as
/// summaries do. One table feeds both renderers, `schema_json`'s `docs`
/// object and the `Examples:` and `Exit codes:` sections of `--help`, so the
/// two cannot drift. Only leaf commands have rows; a group has no
/// `docs.examples` and no help sections.
///
/// All spans borrow static storage owned by the binary.
export struct command_docs {
  /// `(full command path, invocation)` pairs. A command may appear on several
  /// rows; they render in table order.
  std::span<std::pair<std::string_view, std::string_view> const> examples;
  /// `(full command path, space-separated exit codes)` pairs, one row per
  /// leaf, codes in the order they should render.
  std::span<std::pair<std::string_view, std::string_view> const> exit_codes;
  /// The binary-wide meaning of each code a row may name.
  std::span<exit_code_doc const> meanings;
  /// Meanings that differ for one command.
  std::span<exit_meaning_override const> overrides;
};

/// @brief The examples and exit codes `docs` holds for one command.
export struct command_doc_entry {
  /// Example invocations, in table order.
  std::vector<std::string_view> examples;
  /// `(code, meaning)` pairs, in table order. A code with no meaning in the
  /// table carries an empty meaning.
  std::vector<std::pair<int, std::string_view>> exit_codes;
};

/// @brief Resolve the examples and exit codes `docs` holds for `command`.
/// @param docs The binary's table.
/// @param command Full command path, e.g. `planar task update`.
/// @return The entry; both lists are empty for a command with no rows.
export auto docs_for(const command_docs& docs, std::string_view command) -> command_doc_entry;

/// @brief Render the `Examples:` and `Exit codes:` help sections for `command`.
///
/// The text is what `install_docs_footers` hands CLI11 as the footer: an
/// `Examples:` header, one two-space-indented invocation per line, a blank
/// line, an `Exit codes:` header, then `  <code>  <meaning>` per line, with no
/// trailing newline. A section with no rows is omitted.
/// @param docs The binary's table.
/// @param command Full command path.
/// @return The sections, or an empty string for a command with no rows.
export auto render_docs_footer(const command_docs& docs, std::string_view command) -> std::string;

/// @brief Install the `Examples:` and `Exit codes:` sections as the footer of
/// every command `docs` has rows for.
///
/// Walks the tree with `all_nodes` and sets each leaf's footer to
/// `render_docs_footer`; a command without rows keeps no footer. Call it after
/// the whole tree is declared. The spans inside `docs` must outlive the tree.
/// @param root The command tree root.
/// @param docs The binary's table.
export auto install_docs_footers(CLI::App& root, const command_docs& docs) -> void;

/// @brief Emit the catalog with summaries, empty-string defaults, and the
/// per-command `docs.examples` and `docs.exitCodes`.
///
/// Every other `docs` slot stays empty. A command with no rows in `docs`
/// emits `[]` for both.
/// @param root The command tree root.
/// @param summaries See the two-argument overload.
/// @param empty_string_defaults See the three-argument overload.
/// @param docs The binary's examples and exit codes.
/// @return The catalog as a single-line JSON document (no trailing
/// newline).
export auto schema_json(const CLI::App& root, std::span<std::pair<std::string_view, std::string_view> const> summaries,
                        std::span<std::pair<std::string_view, std::string_view> const> empty_string_defaults,
                        const command_docs&                                            docs) -> std::string;

/// @brief What a `schema` invocation asks for.
export struct schema_request {
  /// The command to look up, as the operator typed it; unset selects every command.
  std::optional<std::string> command;
  /// Whether to reduce each command to its full path and summary.
  bool compact = false;
};

/// @brief Declare `--command` and `--compact` on a binary's `schema` node.
/// @param schema_node The `schema` subcommand.
export auto declare_schema_flags(CLI::App& schema_node) -> void;

/// @brief Read the `schema` flags from a parse result.
/// @param args The parsed arguments of the `schema` leaf.
/// @return The request those flags describe.
export auto schema_request_of(const parsed_args& args) -> schema_request;

/// @brief Narrow a full flat catalog to what `request` asks for.
///
/// With no request fields set the catalog is returned unchanged, byte for
/// byte. `--command` selects one command object, copied verbatim from the
/// catalog; the name resolves as the full path (`planar task update`) or
/// relative to the root (`task update`), with runs of whitespace collapsed.
/// `--compact` rewrites every command to `{"command":...,"summary":...}`
/// inside the same envelope with `"layout":"compact"`. Both together emit the
/// one compact row, bare.
/// @param catalog A catalog produced by `schema_json` (or any emitter of the
/// same flat shape, such as `planar-execute`'s).
/// @param request The selection.
/// @return The JSON document without a trailing newline, or a message naming
/// the unknown command.
export auto select_schema(std::string_view catalog, const schema_request& request) -> std::expected<std::string, std::string>;

} // namespace planar::cliapp
