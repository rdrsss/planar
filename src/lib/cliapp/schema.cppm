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
import planar.cliapp.walk;

namespace planar::cliapp {

/// @brief Emit the flat JSON schema catalog for the tree rooted at `root`.
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

} // namespace planar::cliapp
