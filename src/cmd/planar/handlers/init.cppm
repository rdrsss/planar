/// @file init.cppm
/// @brief `planar.cmd.planar.handlers.init` — the `planar init` leaf
/// (plan 996, task 6132).
///
/// Port target: `zig/src/cmd/planar/handlers/init.zig`.
///
/// ## Why this leaf mattered enough to be its own cycle
///
/// The M9 measurement ran the Zig integration corpus against the C++
/// binaries: 41 passing of 651, and `init` alone accounted for 452 of the
/// 609 failures — 74%. Not because `init` is 74% of the surface, but
/// because `harness.registerProject` calls it, so every suite that needs a
/// registered project died on its first line and every assertion after it
/// was unreachable rather than failing. This handler is the one that turns
/// "never ran" into "measured".
///
/// ## What it composes, and why that puts it at layer 3
///
/// Three things that cannot reach each other at layer 2 (D20, decision
/// 947): the runtime's database bootstrap (`context::ensure_db`, which
/// creates the parent directory, applies migrations and runs the
/// schema-version guard), `planar.db.migrate`'s version read-back, and
/// `planar.engine.config.init`'s `register_cwd`. Plus one thing no engine
/// bucket owns at all — a `git` subprocess.
///
/// ## The git remote is the field this cycle exists to not get wrong
///
/// Task 6128 is the cautionary case: `planar capture session` exits 0 with
/// stdout byte-identical to the oracle while writing NULL `repo_root` and
/// NULL `head_sha_at_start`, because the engine supports the fields and
/// the handler never passes them. Every other port gap announces itself
/// with exit 64; that one looks exactly like success.
///
/// `projects.git_remote` has precisely that shape here, and it is
/// invisible in the common case: a scratch directory has no `origin`, so a
/// handler that never probed at all would still match the oracle
/// byte-for-byte in every fixture that does not first run `git init` and
/// `git remote add`. The probes that pinned the real contract were run
/// against the oracle in a pinned arena:
///
///   git repo + `origin`      -> `projects.git_remote` set AND a
///                               `"git_remote"` key appended to the JSON
///   git repo + `upstream` only -> NULL, and the JSON key ABSENT
///   git repo, no remotes     -> NULL, key absent
///   `.git/` with a hand-written config but no real repository -> NULL
///   real repo run with `PATH=` emptied                        -> NULL
///
/// The last two are what establish that the oracle SHELLS git rather than
/// parsing `.git/config`: both are cases where a config parser would have
/// found a URL and `git` itself refuses. Confirmed against
/// `zig/src/cmd/planar/handlers/init.zig`'s `gitRemoteOrigin`, which runs
/// `git -C <cwd> remote get-url origin`, trims ASCII whitespace, and
/// treats a non-zero exit, a signal, a spawn failure, or empty output all
/// as "no remote". `probe_git_origin` below reproduces every one of those
/// arms, and `handlers.t.cpp` asserts on the ROW, not only on stdout, so a
/// regression to the 6128 shape fails a test instead of passing one.
///
/// One of those arms needed a fixture nobody would have built by accident.
/// Deleting the exit-status check from `probe_git_origin` SURVIVED the
/// whole suite on its first break-probe: real `git remote get-url` writes
/// its failures to stderr and nothing to stdout, so the empty-output check
/// subsumes the status check for every shape a real repository can produce.
/// The two are still not equivalent, and the oracle checks the status
/// first, so `init.t.cpp` now builds the discriminating shape directly — a
/// `git` on PATH that prints a plausible URL and exits 1 — and the mutant
/// dies. The check stays because the oracle has it (D2), not because it
/// was reachable by the fixtures that already existed.
///
/// ## `--allow-no-repo` is accepted and does nothing, deliberately
///
/// The flag is declared — its help text promises to "Allow initialization
/// outside a git repo" — and the oracle's handler never reads it. Probed
/// rather than assumed: `init` in a non-git scratch directory WITHOUT the
/// flag exits 0 and registers the project, and passing the flag produces
/// byte-identical output. Reproducing the no-op is D2; adding the refusal
/// the help text implies would refuse a case the oracle accepts.
///
/// ## The declaration stays generated
///
/// Unlike the seven verbs hand-transcribed into `tree.cpp`, `init`'s node
/// keeps coming from `planar.cmd.planar.surface`'s generated inventory —
/// only its entry in `unported_paths()` is dropped, so the
/// `not_implemented` binding gives way to this handler. `apply_surface`
/// skips nodes that already exist, so either placement dispatches
/// identically; the generated one is derived from the oracle's own
/// `schema` catalog and is what `catalog_parity` checks continuously,
/// whereas hand-transcribing six flags and their help strings would
/// reintroduce exactly the transcription risk the generator exists to
/// remove.
module;

export module planar.cmd.planar.handlers.init;

import std;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief `planar init` — create and migrate the database, then (unless
/// `--skip-project`) register the operator's working directory as a
/// project.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure as a `domain_error`.
export auto init(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Best-effort `git -C <dir> remote get-url origin`, trimmed.
///
/// Exported for test, because the negative arms are the ones that matter
/// and none of them is observable from stdout in a fixture without a
/// repository. Never throws and never fails loudly: git missing, `dir` not
/// a repository, no `origin` configured, and empty output are all reported
/// the same way the oracle reports them — as no answer.
/// @param dir The directory to probe, passed to git as `-C`.
/// @return The trimmed remote URL, or unset.
export auto probe_git_origin(const std::filesystem::path& dir) -> std::optional<std::string>;

} // namespace planar::cmd::handlers
