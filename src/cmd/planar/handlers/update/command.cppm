/// @file src/cmd/planar/handlers/update/command.cppm
/// @brief `planar.cmd.planar.handlers.update` — the `planar update` leaf (plan
/// 1122 M3, task rel-update-verb; tech spec 677, "The update verb";
/// decisions 1326, 1328, 1331, 1334 and 1337).
///
/// `planar update` replaces the installation at `~/.planar` (or
/// `$PLANAR_HOME`, the root the installer it hands off to uses) with a
/// published release. It opens no database: the database probe, migration and
/// queue warning belong to the installer.
///
/// ## `--check`
///
/// Reads the installed release from `<root>/release.json` (`none` when there
/// is none), fetches `<base>/latest/download/VERSION`, validates it as a
/// release tag and prints `installed <v> latest <v>`. Exit 0 when they are
/// equal, `exit_update_available` (10) when they differ, 1 on a fault. It
/// takes no lock and changes nothing.
///
/// ## A plain run, or `--version <tag>`
///
///   1. Takes the common mutation ownership (`update.lock`, the native port of
///      `scripts/install-lib/mutation-lock.sh`) as operation `update`, with its
///      temporary directory `<root>/.planar-update/<name>` recorded before the
///      directory is created. A competing install, update or uninstall refuses
///      naming its owner. A proven-dead updater's recorded directory is
///      removed (and nothing else matching a name pattern).
///   2. Under that ownership reads the recovery journal: a `mutating` or
///      `uninstalling` journal is reported as an incomplete installation at
///      exit 1 with its durable retry command, before any verdict about the
///      current release. The verb never replays recovery itself; the
///      bootstrap's pinned recovery (`PLANAR_EXPECT_RECOVERY`) is the one
///      replay path. `prepared` and aborted journals leave the previous
///      completed release authoritative.
///   3. Resolves the tag as the bootstrap does and, when it equals the
///      installed release of a completed install, reports no changes at exit 0.
///   4. Downloads `SHA256SUMS` and the asset from `<base>/download/<tag>/`
///      through `planar.http`'s download policy, each with a size bound, into
///      the temporary directory; selects exactly one checksum record and
///      verifies with `planar.sha256`.
///   5. Lists, then extracts the archive with the system `tar` under the
///      bootstrap's entry rules, checks the bundle's `release.json` version,
///      the glibc floor on Linux, and refuses a bundle whose `schema_version`
///      is below this binary's own (decision 1326).
///   6. `exec`s `bash <bundle>/install.sh --prebuilt <bundle> --cleanup <tmp>`
///      as an argument vector with `PLANAR_MUTATION_HANDOFF=<G>:<nonce>`:
///      ownership is never released before the exec, and the installer adopts
///      it (exec keeps the pid and start time). The installer removes the
///      temporary directory and releases ownership when it ends.
///
/// Every refusal before the exec, and a failed exec, removes the temporary
/// directory and releases ownership. Refusals reuse the bootstrap's messages
/// (`scripts/get-planar.sh`).
///
/// ## SIGINT and SIGTERM
///
/// From just before it takes ownership until the exec, a plain run catches
/// SIGINT and SIGTERM (one that was ignored when it started, such as a
/// background job's SIGINT, stays ignored). The handler only records the
/// signal. The run stops at its next checkpoint, or at once inside a download
/// through `planar.http`'s cancel hook, removes the temporary directory and
/// releases ownership (both with the two signals blocked, so a second signal
/// cannot cut them short), restores the previous dispositions and exits
/// 128+signo (130 or 143) through `passthrough_code`. Immediately before the
/// exec, with both signals blocked, it gives up when one was caught and
/// otherwise restores the previous dispositions: a signal from there on is
/// never lost, and acts as it would on any process. Before the exec it ends
/// this one like a KILL; after it the installer, which then owns the directory
/// and ownership, handles it. A KILL leaves both; the next owner proves the
/// updater dead and removes exactly its recorded directory. `--check` holds
/// nothing and catches nothing.
///
/// ## The host seam
///
/// `update_host` carries the three things a test cannot get from a real run:
/// the platform, the first line of `ldd --version`, and the `exec` itself.
/// `native_host()` is the only value production uses (`update` passes it);
/// the seam is reachable only by calling `run_update` in-process, never from
/// the command line or the environment.
module;

export module planar.cmd.planar.handlers.update;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief One environment change for the exec'd installer: set to a value, or
/// removed when the value is unset.
export using env_change = std::pair<std::string, std::optional<std::string>>;

/// @brief The installer invocation `planar update` hands off to.
export struct exec_request {
  std::string              program; ///< The resolved `bash` executable.
  std::vector<std::string> argv;    ///< The argument vector, `argv[0]` included.
  std::vector<env_change>  env;     ///< Changes to this process's environment.
};

/// @brief The host facts and the exec, injectable for in-process tests.
export struct update_host {
  /// @brief The bundle platform (`macos-arm64`, `linux-x86_64`), or the
  /// bootstrap's refusal for an unsupported host.
  std::function<std::expected<std::string, std::string>()> platform;
  /// @brief The first line of `ldd --version`, or unset when `ldd` is absent.
  std::function<std::optional<std::string>()> ldd_first_line;
  /// @brief Replace this process with the installer. Returns only on failure,
  /// with the `errno`; a test double may return success to stand for a
  /// completed hand-off.
  std::function<std::expected<void, int>(const exec_request&)> exec;
};

/// @brief The real host: `uname`, `ldd --version`, and `execve`.
/// @return The host.
export auto native_host() -> update_host;

/// @brief Run `planar update` against `host`.
/// @param ctx The invocation context (environment, cwd, streams).
/// @param args The parsed arguments.
/// @param host The host facts and exec.
/// @return Success; exit 10 through `passthrough_code` for `--check` with an
/// update available; 128+signo through `passthrough_code` for a plain run
/// stopped by SIGINT or SIGTERM; otherwise the failure.
export auto run_update(context& ctx, const cliapp::parsed_args& args, const update_host& host) -> handler_result;

/// @brief Handle `planar update` on the real host.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return As `run_update`.
export auto update(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `update` leaf.
/// @param root The root app to attach it to.
export auto declare_update(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
