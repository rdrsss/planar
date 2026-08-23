/// @file schema.cppm
/// @brief `planar.cli.schema` — emits the deterministic flat JSON catalog
/// `tools/cli_usage_lint` consumes (`<bin> schema`).
///
/// Behavior-preserving port of
/// zig/vendor/etcli-zig/src/cli/schema.zig::json (D2, D9). The Zig source
/// builds this string entirely at comptime because it is `.rodata` for a
/// fixed comptime command tree; this port has no comptime tree (`cmd` is
/// ordinary runtime data — see cmd.cppm/flag.cppm's file comments) so
/// `json()` is a plain runtime walk over `all_nodes`/
/// `collect_inherited_flags`, matching the key set, key order, and
/// zero-whitespace JSON formatting of the Zig emitter field-for-field.
///
/// Divergences from the Zig oracle, forced by fields this port's `cmd`/
/// `flag`/`positional` types do not carry (see cmd.cppm and flag.cppm's
/// file comments — `deprecated`, per-node `doc`, and per-flag/positional
/// `completion` are all engine/cmd or comptime-reification concerns this
/// task's tree-only port never modeled):
///   - `"deprecated"` is always emitted as `null` (both on commands and on
///     flags) — this port carries no `Deprecation` field to source it
///     from.
///   - `"docs"` is always emitted as the Zig `Doc{}` empty-default shape
///     (`{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],
///     "bugs":[],"authors":[],"homepage":"","license":"","copyright":"",
///     "version":"","sourceUrl":""}`) — this port's `cmd` has no `doc`
///     field. This still byte-matches the oracle for every command in the
///     modeled subset because none of them declare doc metadata; a command
///     that did would diverge here until a `doc` field is added.
///   - `"completion"` (on both flags and positionals) is always emitted as
///     `{"kind":"none","values":[]}` — this port's `flag`/`positional`
///     types carry no `completion` field. Matches the oracle for the
///     modeled subset (none of its flags/positionals declare completion).
///   - `"commandTree"` (the opt-in nested-tree view, `include_command_tree`)
///     is never emitted — this port's default-`Options` walk mirrors the
///     Zig default (`include_command_tree = false`), so the top-level key
///     is simply absent, matching the oracle byte-for-byte at the default
///     options the `schema` verb actually calls with.
///
/// The `"commands"` array itself is necessarily a subset of the real
/// binaries' catalogs: this port only models the command trees the
/// caller builds and passes to `json()` (see help.t.cpp/parser.t.cpp's
/// `make_planar_root`/`make_planar_agent_root` fixtures for the task/task
/// add/task done/planar-agent fail subset this task's own test proves
/// against). A parent node's `"subcommands"` list reflects only the
/// children present in the caller's tree, so `"task"`'s subcommand list
/// here is `["add","done"]` rather than the full fifteen-subcommand list
/// the real `planar task` node exposes — full-surface parity requires the
/// full command tree to be ported (out of this task's scope; see the
/// module's test file for what that entails).
module;

export module planar.cli.schema;

import std;
import planar.cli.flag;
import planar.cli.cmd;

namespace planar::cli {

/// @brief Emit the flat JSON schema catalog for the tree rooted at `root`,
/// at the Zig emitter's default `Options` (inherited flags included, docs
/// included as empty defaults, env metadata included, no nested
/// `commandTree`, hidden/deprecated-excluded nodes and flags skipped since
/// this port models no `deprecated` field — deprecated inclusion is
/// therefore always effectively on, matching the Zig default
/// `include_deprecated = true`).
/// @param root The command tree root (its own `name` becomes the JSON
/// `"root"` value and the first path segment of every `"command"` string).
/// @return The catalog as a single-line JSON document (no trailing
/// newline — callers matching `handlers/schema.zig`'s `planar schema`
/// verb append one at the write site, not here).
export auto schema_json(cmd const& root) -> std::string;

} // namespace planar::cli
