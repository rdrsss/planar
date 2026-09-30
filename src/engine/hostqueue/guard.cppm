/// @file guard.cppm
/// @brief `planar.engine.hostqueue.guard` — the command guard of `queue run`
/// (plan 1080, task hq-command-guard; tech spec 647 § Which commands may be
/// queued).
///
/// The queue exists to serialise builds and tests. A model launcher queued
/// behind them would hold a slot for as long as a conversation lasts, so
/// `check_command` refuses a command whose program is on a short list of
/// launchers (`model_launchers()`). The list is purpose-built; it is not
/// derived from `planar-execute`'s host-function deny list.
///
/// ## Which program is checked
///
/// The guard takes the BASENAME of the program: `/usr/local/bin/claude` and
/// `claude` are the same program, and a name that merely contains a listed
/// word (`codex-lint-report`, `claude_fixture`) is a different one. The names
/// are matched WITHOUT REGARD TO ASCII CASE (`Claude`, `CODEX`): macOS's default
/// volumes are case-insensitive, so a PATH lookup of `Claude` runs `claude`,
/// and an exact comparison would be walked around by changing a letter. The
/// refusal reports the listed spelling. The same folding applies to `env`.
/// Non-ASCII bytes compare exactly. Before the
/// program is chosen the guard skips what would run in front of it:
///
///   * leading `NAME=value` assignments;
///   * a leading `env` (by basename, so `/usr/bin/env` counts) together with
///     its options and any assignments that follow it: `-i`, `-0`, `-v`,
///     `-u NAME` / `-uNAME` / `--unset=NAME` / `--unset NAME`,
///     `-C DIR` / `-CDIR` / `--chdir=DIR` / `--chdir DIR`, `-P PATH` / `-PPATH`,
///     `-a NAME` / `-aNAME` / `--argv0=NAME` / `--argv0 NAME`, FreeBSD's
///     `-L USER` / `-LUSER` and `-U USER` / `-UUSER` (macOS and GNU env reject
///     them, so such a command fails inside env before anything runs; consuming
///     the value is harmless, and keeps the user from being read as the
///     program), clustered short
///     options such as `-iu NAME`, and `--` (which ends the options);
///   * `-S STRING` / `-SSTRING` / `--split-string=STRING` /
///     `--split-string STRING`, whose string is split on white space and
///     stands in front of the remaining words, since `env -S 'claude -p x'`
///     starts `claude`.
///
/// The skipping repeats, so `env env claude` and `env A=1 env -i claude` are
/// refused too. A command that is only `env`, or `env` with nothing to run, has
/// no program and is not refused.
///
/// The guard is a denylist by design. It does not look inside `sh -c` strings
/// or interpreters: a wrapper the operator writes is the operator's choice.
///
/// Pure: no I/O, no process state. Error boundary: a refusal is a value
/// (`std::expected<void, guard_refusal>`); nothing throws.

module;

export module planar.engine.hostqueue.guard;

import std;

namespace planar::engine::hostqueue {

/// @brief The program names `queue run` refuses, in the order the tech spec
/// lists them.
/// @return `claude`, `codex`, `gemini`, `copilot`, `aider`, `opencode` and
/// `cursor-agent`.
export auto model_launchers() -> std::span<const std::string_view>;

/// @brief Why a command was refused.
export struct guard_refusal {
  /// @brief The listed name the command's program matched (its basename,
  /// compared without regard to ASCII case), spelled as listed.
  std::string program;
};

/// @brief Decides whether `argv` may be queued.
/// @param argv The full argument vector, program first. An empty vector has no
/// program and is not refused.
/// @return Success, or the launcher the command would start, in its listed
/// spelling.
export auto check_command(std::span<const std::string> argv) -> std::expected<void, guard_refusal>;

} // namespace planar::engine::hostqueue
