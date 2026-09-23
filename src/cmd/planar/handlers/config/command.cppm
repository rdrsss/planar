/// @file src/cmd/planar/handlers/config/command.cppm
/// @brief `planar.cmd.planar.handlers.config` — all five `planar config`
/// leaves (plan 996, task 6259).
///
/// Port target:
/// `zig/src/cmd/planar/handlers/config/{show,edit,validate,init,path}.zig`.
///
/// The engine underneath landed complete in commit 82820b7 (tasks
/// 6076/6080/6081/6082): models/routing/roles resolution, `candidates` on
/// `value_with_source`, line/column-carrying TOML diagnostics, and the
/// table-header normalisation that lets a super-table follow its own
/// sub-table. This file is WIRING over that, plus the two things the engine
/// deliberately does not own: the config-file PATH (which is a process
/// concern, not a resolution concern) and the two starter blobs.
///
/// ## None of the five opens SQLite
///
/// `ctx.db().opened()` is pinned false after every one of them. That is a
/// real contract rather than an accident: `config init` and `config path`
/// are what an operator runs BEFORE `planar init`, on a machine with no
/// database at all, and `config validate` is the thing you reach for when
/// something is already wrong. A version of these that opened SQLite would
/// also MIGRATE it — the runtime applies pending migrations on first use —
/// so a stale binary running `config path` could push the operator's
/// database past what every other installed binary supports.
///
/// ## `$HOME` and `$PLANAR_CONFIG_PATH`. NOT `$PLANAR_HOME`, NOT `$PLANAR_DB`
///
/// The path is `$PLANAR_CONFIG_PATH` (leading `~` expanded against `$HOME`)
/// else `$HOME/.planar/config.toml`, resolved by
/// `planar.cmd.planar.cli_log`'s `resolve_config_path` — which was ported
/// from `handlers/config/path.zig` and whose own header says `config path`
/// should call IT rather than grow a second copy. It does.
///
/// Oracle-confirmed in a pinned arena: with `HOME` and `PLANAR_HOME`
/// pointing at different directories, `config path` printed the one under
/// `HOME`.
///
/// ## THE TWO STARTER BLOBS ARE DIFFERENT, AND THAT IS THE ORACLE'S DEFECT
///
/// `config init` and `config edit` both write a starter file when none
/// exists, and the Zig tree carries the blob TWICE — once in `init.zig`,
/// once in `edit.zig` — with a comment in the second claiming it "mirrors
/// init.zig's starter_config exactly". It does not. `init.zig`'s copy grew
/// an eight-line `[models.codex]` / `[roles]` example block (plan 540) that
/// was never copied back into `edit.zig`'s.
///
/// Measured against the built oracle: `config init` writes 1267 bytes,
/// `config edit` writes 1048, and the diff is exactly those eight lines.
/// Both are reproduced verbatim as two separate constants. Collapsing them
/// to one — the obvious tidy — would change what one of the two verbs
/// writes to the operator's disk, which is a behaviour change wearing a
/// refactor's clothes (D2). The duplication is deliberate and labelled.
///
/// ## `config show --format` IS DECLARED AND NEVER READ
///
/// The flag exists in the tree with a `"text"` default and the Zig handler
/// never looks at it: `--format json` prints the plain text listing.
/// Only `--json` switches the encoding. Oracle-captured and pinned rather
/// than implemented — an operator scripting `--format json` gets text, and
/// a port that "fixed" it would silently change what their pipeline reads.
///
/// ## Three named divergences in `config validate`'s stderr
///
/// `config validate`'s FORMAT, exit codes, message set, error ORDER and
/// stream are all reproduced exactly. Its TOML-diagnostic BYTES are not,
/// and cannot be, because the two trees do not share a TOML parser — Glaze
/// owns it here (D12) and `zig/src/engine/config/parse.zig` is a
/// hand-rolled subset there. All three are pinned AS divergences in
/// `config_leaves.t.cpp`, each in a test that names it, so the difference
/// is asserted rather than merely tolerated:
///
///   1. DUPLICATE KEY IN ONE TABLE. `vendor = "a"` twice is a hard error
///      here and last-wins in the oracle. Decision 964: the TOML
///      specification makes it an error, Glaze is conformant, the oracle is
///      not, and this stays strict. `config validate` is where an operator
///      SEES it — the oracle exits 0.
///   2. SEMANTIC-REJECTION COLUMN. Both trees report the same line and the
///      same message for `x = 1.5`; the oracle's column points at the
///      offending character and this tree's at the value's first
///      character. `toml_error` calls this "equivalent, not identical".
///   3. SYNTAX-ERROR MESSAGE TEXT. Different grammars produce different
///      prose; this tree emits Glaze's `error_code` name. There is no
///      version of this that matches without reimplementing `parse.zig`,
///      which D12 exists to avoid.
///
/// Chasing any of the three would mean either abandoning Glaze or
/// abandoning TOML conformance. Naming them is the honest option and the
/// one the task brief anticipated.
module;

export module planar.cmd.planar.handlers.config;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar config show [--effective] [--raw] [--defaults]
/// [--scope <slug>] [--format <fmt>] [--json]`.
///
/// Flag precedence, in the oracle's own order and not a tidied one:
/// `--defaults` beats everything (it prints the embedded file and returns
/// before the config file is even read), then `--raw`, then the resolved
/// listing. `--effective` and `--json` are the SAME switch for the listing
/// body — `--json` alone implies provenance — so the only two shapes are
/// "key = value" and the provenance forms.
///
/// An EMPTY `--scope` is no scope at all, not an association named "".
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or a `generic_failure` (exit 1) when the path cannot be
/// resolved, the file cannot be read, or it does not parse. Never opens the
/// database.
export auto config_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar config edit`.
///
/// Writes the SHORTER starter blob when the file is absent (see this
/// module's header), then execs the editor ON THE CONFIG FILE ITSELF —
/// never a temp copy, so there is no write-back step and no way for an
/// editor crash to lose the operator's file.
///
/// A non-zero editor exit is a FAILURE here (exit 1, `error: editor exited
/// with code N`), which is the opposite of `planar.cmd.planar.editor`'s
/// `invoke` contract and is the oracle's behaviour.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or a `generic_failure` (exit 1). Never opens the
/// database.
export auto config_edit(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar config validate`.
///
/// Four ordered steps, and the order is observable: TOML parse (a failure
/// returns IMMEDIATELY, so a file that is both unparseable and carries a
/// literal secret reports only the parse error), then the line-oriented
/// sensitive-literal scan, then the `external.github-issues.auth`
/// cross-checks, then the work-type routing cross-check. Steps 2-4
/// ACCUMULATE: every finding is reported and the exit is 1 once, at the
/// end.
///
/// The routing cross-check skips any `routing.<vendor>.<tier>.<work-type>`
/// whose tier has NO `models.<vendor>.<tier>` entry — the oracle's `orelse
/// continue`, confirmed against the built binary with a
/// `routing.codex.medium.cli` naming a model that exists nowhere: exit 0.
/// Validating it would refuse configs the oracle accepts.
///
/// Every message goes to STDERR; the only stdout this verb ever writes is
/// `config validate: ok`. See this module's header for the three named
/// divergences in step 1's bytes.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or a `generic_failure` (exit 1) carrying the complete
/// multi-line stderr payload. Never opens the database.
export auto config_validate(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar config init`.
///
/// Idempotent and NOT an error when the file exists: `config init:
/// already exists <path>` at exit 0. Writes the LONGER starter blob (see
/// this module's header) and creates missing parent directories.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or a `generic_failure` (exit 1) when the path cannot be
/// resolved or the file cannot be written. Never opens the database.
export auto config_init(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar config path`.
///
/// Prints the resolved path and nothing else. Does NOT check whether the
/// file exists — an absent config still has a path, and printing it is how
/// an operator finds out where to create one.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or a `generic_failure` (exit 1) when neither
/// `$PLANAR_CONFIG_PATH` nor `$HOME` yields a path. Never opens the
/// database.
export auto config_path(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `config` command tree on `root`.
///
/// The CLI declaration for every `config` node, colocated with the
/// `config_*` handlers above (plan 1051, M11.3e — decision 1068). None was
/// hand-declared in `tree.cpp`.
///
/// Four of the five leaves — `edit`, `validate`, `init`, `path` — declare
/// NOTHING: no flag, not even `--json`. They are written without a local
/// `CLI::App*` for that reason, since naming a pointer nothing then uses
/// is a warning rather than documentation.
///
/// `show --format` carries the declared default `text` that the catalog
/// reports; as config.cppm's header records, the handler ignores the flag's
/// value. The declaration keeps it because the surface is the contract.
/// @param root The root app to attach the `config` group to.
export auto declare_config(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
