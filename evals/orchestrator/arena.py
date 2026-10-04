#!/usr/bin/env python3
"""Scratch process-environment arena for eval harness subprocess isolation.

An arena is the scratch tree a live or lifecycle eval run (or a trial of
one) executes in: its own `HOME`, `PLANAR_DB`, `PLANAR_WORKBENCH_ROOT`,
`PLANAR_CONFIG_PATH`, and vendor config directories (`CLAUDE_CONFIG_DIR`,
`CODEX_HOME`), all under one root. `src/cmd/parity_harness.hpp::make_arena()`
is the C++ reference this module mirrors for the Python side.

`make_arena()` builds the scratch tree and the subprocess environment from
an explicit pass-through allowlist — never `os.environ.copy()` — so a run
cannot silently inherit an unrelated credential or path from the operator's
shell. `assert_isolated()` is the fail-closed check every live and
lifecycle run calls before the host process starts (decision D2, tech spec
619 component C1): every arena-owned variable must be set and must resolve
under the arena root, or the run refuses with the offending key named.

`stage_vendor_config()` (Planar question 983) then STAGES the vendor's
auth and settings surfaces into the scratch `CLAUDE_CONFIG_DIR` /
`CODEX_HOME` as symlinks back to the operator's real install (the codex CLI
reads `~/.codex/auth.json`), while every WRITE the host makes to a NEW path
(session state, history) lands in the scratch dir untouched. The Planar
surface itself is NOT taken from the operator's install: when given a
`repo_root` it copies the checkout's single `skills/planar/` directory into
`<claude-config>/skills/planar` and `<home>/.agents/skills/planar`, and each
`agents/planar-<role>.md` into `<claude-config>/agents/` (Claude) or as a
generated `<codex-home>/agents/planar-<role>.toml` (Codex), so a run
exercises the checkout under test and never a stale install. It is
a fail-closed, allowlisted STAGING step, not a broadening of the arena: it
never grants read/write access to anything outside the named surfaces below,
and `assert_isolated()` continues to validate the arena env var VALUES
(the scratch directories) — the symlinks living inside them, pointing back
out to the real install, are the intended exception.

`assert_vendor_auth()` (Planar artifact 626 / task 6872) is the last
fail-closed check before a host spawns: staging a vendor's read surfaces
is not the same as authenticating it, and Keychain-backed `claude` login
does not follow into a scratch `CLAUDE_CONFIG_DIR` at all. Every
live/lifecycle call site in `evals/orchestrator/harness.py` runs the same
four steps in order — `make_arena()` -> `assert_isolated()` ->
`stage_vendor_config()` -> `assert_vendor_auth()` — before any vendor host
process starts. The planning harness (`evals/planning/harness.py`, task
hh-planning-grader-split) runs the same four steps for its live
`draft-quality` trial loop: its drafter now invokes the installed
`planar-planner` agent by name, so it needs that agent and the `planar`
skill staged into the scratch `CLAUDE_CONFIG_DIR` the same way an
orchestrator run does. Its `--dry-run` path (every
kind) and its grader call (plain prose, no slash command) never need
`stage_vendor_config()`, so only the live drafter call site pairs it with
`assert_vendor_auth()`.

Callers own an arena's lifecycle. This module never deletes a root; a
run's `--keep` / failure rules decide whether the artifact directory (and
every trial arena under it) is retained or removed as a unit.
"""

from __future__ import annotations

import json
import os
import shutil
from pathlib import Path
from typing import Mapping

# Exact-name variables copied through unchanged when present in the source
# environment.
PASSTHROUGH_EXACT_VARS: tuple[str, ...] = ("PATH", "TERM", "TMPDIR", "LANG")

# Prefixes copied through unchanged: provider credentials and locale
# variants vary by vendor CLI and by operator locale, so they are matched
# by prefix rather than enumerated one by one.
#
# The explicit vendor auth surfaces `assert_vendor_auth()` checks all live
# under these prefixes: claude auth is `CLAUDE_CODE_OAUTH_TOKEN` (minted by
# `claude setup-token`, prefix `CLAUDE_CODE_`) or `ANTHROPIC_API_KEY`
# (prefix `ANTHROPIC_`); codex auth is `OPENAI_API_KEY` (prefix
# `OPENAI_`) or a staged `$CODEX_HOME/auth.json` (not an env var — see
# `CODEX_STAGED_SURFACES`). Keychain-backed `claude` login does NOT count:
# it is bound to the default `CLAUDE_CONFIG_DIR` and cannot be staged into
# a scratch one (Planar artifact 626 / task 6872).
PASSTHROUGH_PREFIXES: tuple[str, ...] = (
    "LC_",
    "ANTHROPIC_",
    "OPENAI_",
    "CLAUDE_CODE_",
)

# The arena-owned variables every live/lifecycle subprocess environment
# must carry, each resolving under the arena root. Order matches the tech
# spec's C1 component listing.
ARENA_ENV_VARS: tuple[str, ...] = (
    "HOME",
    "PLANAR_DB",
    "PLANAR_WORKBENCH_ROOT",
    "PLANAR_CONFIG_PATH",
    "CLAUDE_CONFIG_DIR",
    "CODEX_HOME",
)


class ArenaIsolationError(RuntimeError):
    """A subprocess environment leaked a path outside its arena root.

    Raised by `assert_isolated()` before any vendor host process starts.
    """


class VendorStagingError(RuntimeError):
    """A REQUIRED vendor read-surface is missing from the real install.

    Raised by `stage_vendor_config()` before any vendor host process
    starts. An OPTIONAL surface that is missing (e.g. no `settings.json`)
    is skipped silently — see `CLAUDE_STAGED_SURFACES` /
    `CODEX_STAGED_SURFACES`.
    """


class VendorAuthError(RuntimeError):
    """No usable auth surface for `vendor` is present in the arena env.

    Raised by `assert_vendor_auth()` before any vendor host process
    starts (Planar artifact 626 / task 6872: a live run staged
    `agents/`/`commands/` correctly but Claude answered "Not logged in";
    Keychain-backed `claude` login is bound to the default
    `CLAUDE_CONFIG_DIR` and does not follow a scratch one). The message
    names the missing variable or staged file and how to provide it; it
    NEVER includes a credential VALUE, staged or otherwise.
    """


# Each vendor's read surfaces staged from the operator's real install into
# the scratch `CLAUDE_CONFIG_DIR` / `CODEX_HOME`, mapped to whether that
# surface is REQUIRED (missing -> fail closed before the host starts) or
# OPTIONAL (missing -> skipped silently). Deliberately a narrow, explicit
# allowlist rather than staging the whole real directory: anything not
# named here — in particular session/history/state — is never staged, so
# it stays exactly where the arena already puts it (scratch, empty, never
# touching the operator's real state).
#
# Skills, agents and commands are deliberately absent: the Planar surface is
# staged from the repository checkout by `stage_planar_surface()`, never
# from the operator's install. Claude has no required real-install surface.
# `auth.json` is required for codex: this is how the installed codex CLI
# authenticates on this machine (there is no OS-keychain fallback the way
# there is for claude).
CLAUDE_STAGED_SURFACES: dict[str, bool] = {
    "settings.json": False,
    "CLAUDE.md": False,
    # Present on some installs, absent on others depending on auth method
    # (e.g. OS keychain vs. a stored credentials file); never required,
    # since claude auth is documented to work via the OS keychain too.
    ".credentials.json": False,
    # `plugins/` is deliberately EXCLUDED, not just optional: Claude
    # rewrites `~/.claude/plugins/.last_inuse_sweep` and
    # `known_marketplaces.json` on ordinary session start, so a symlinked
    # `plugins/` would leak a host write straight into the operator's real
    # install — the one thing q983's "every write lands in scratch"
    # promise exists to prevent. The eval never needs it staged.
}

CODEX_STAGED_SURFACES: dict[str, bool] = {
    "auth.json": True,
    "config.toml": False,
    "AGENTS.md": False,
    "rules": False,
    # `plugins/` is deliberately EXCLUDED, not just optional: the codex CLI
    # writes into its own `plugins/` state on ordinary use, same as
    # claude's — see the CLAUDE_STAGED_SURFACES comment above. Never
    # staged, so it stays scratch-only like every other write surface.
}

VENDOR_STAGED_SURFACES: dict[str, dict[str, bool]] = {
    "claude": CLAUDE_STAGED_SURFACES,
    "codex": CODEX_STAGED_SURFACES,
}

# Deliberately NOT staged, ever: session/history/state. These are exactly
# what must stay in scratch so a run's own activity never lands in — or
# reads from — the operator's real session history. Listed here only so a
# test can assert none of them ever appear as a key in the maps above; the
# maps being an explicit allowlist is what actually enforces this.
EXCLUDED_STATE_SURFACES: tuple[str, ...] = (
    "sessions",
    "history.jsonl",
    "projects",
    "logs",
    "cache",
    "tmp",
    "memories",
)

# Where each vendor's REAL install lives: the env var checked first, and
# the default under the operator's real home used when that var is unset.
VENDOR_SOURCE_ENV_VAR: dict[str, str] = {
    "claude": "CLAUDE_CONFIG_DIR",
    "codex": "CODEX_HOME",
}
VENDOR_SOURCE_DEFAULT: dict[str, str] = {
    "claude": ".claude",
    "codex": ".codex",
}

# The arena-owned env var whose scratch value is this vendor's staging
# TARGET (the same names as the source env vars above, by design — the
# scratch env reuses the vendor's own variable name).
VENDOR_ARENA_ENV_VAR: dict[str, str] = VENDOR_SOURCE_ENV_VAR


def make_arena(root: Path, base_env: Mapping[str, str] | None = None) -> dict[str, str]:
    """Build a scratch subprocess environment rooted at `root`.

    Creates `home/`, `workbench/`, `config/`, `claude-config/`, and
    `codex-home/` under `root` and returns a full subprocess environment:
    the pass-through allowlist (`PASSTHROUGH_EXACT_VARS` /
    `PASSTHROUGH_PREFIXES`) copied from `base_env` (the real process
    environment when omitted), plus the six `ARENA_ENV_VARS` pointed at
    the created paths. No other key from the source environment survives.

    The caller owns `root`'s lifecycle: this function only creates
    directories, it never removes them.
    """
    resolved_root = root.resolve()
    home_dir = resolved_root / "home"
    workbench_dir = resolved_root / "workbench"
    config_dir = resolved_root / "config"
    claude_config_dir = resolved_root / "claude-config"
    codex_home_dir = resolved_root / "codex-home"
    for directory in (home_dir, workbench_dir, config_dir, claude_config_dir, codex_home_dir):
        directory.mkdir(parents=True, exist_ok=True)

    source = base_env if base_env is not None else os.environ
    env: dict[str, str] = {}
    for key in PASSTHROUGH_EXACT_VARS:
        value = source.get(key)
        if value is not None:
            env[key] = value
    for key, value in source.items():
        if key in env:
            continue
        if any(key.startswith(prefix) for prefix in PASSTHROUGH_PREFIXES):
            env[key] = value

    env["HOME"] = str(home_dir)
    env["PLANAR_DB"] = str(resolved_root / "planar.db")
    env["PLANAR_WORKBENCH_ROOT"] = str(workbench_dir)
    env["PLANAR_CONFIG_PATH"] = str(config_dir / "config.toml")
    env["CLAUDE_CONFIG_DIR"] = str(claude_config_dir)
    env["CODEX_HOME"] = str(codex_home_dir)
    return env


def assert_isolated(env: Mapping[str, str], root: Path) -> None:
    """Fail closed unless every arena-owned variable resolves under `root`.

    Every live and lifecycle run calls this before the host process
    starts (decision D2). Checks each of `ARENA_ENV_VARS` individually so
    the raised message names the offending key rather than reporting a
    generic isolation failure; an unset variable is refused the same way
    as one pointing outside the arena.

    This generalizes the former `assert_isolated_db(path, created_by_eval)`
    in `evals/planning/harness.py`: a database path under an arena root
    that only `make_arena()` created is, by construction, one this eval
    created — the same guarantee `created_by_eval` asserted directly.
    """
    resolved_root = root.resolve()
    for key in ARENA_ENV_VARS:
        value = env.get(key)
        if not value:
            raise ArenaIsolationError(f"arena isolation violated: {key} is not set")
        # A relative value would resolve against the CURRENT process's cwd,
        # not the arena: it could accidentally land under the root (masking
        # the leak) or anywhere else, and either way the value is not the
        # self-contained, portable path an arena-owned variable must be.
        # Refuse it explicitly rather than letting `Path.resolve()` paper
        # over the ambiguity.
        if not Path(value).is_absolute():
            raise ArenaIsolationError(
                f"arena isolation violated: {key}={value} is not an absolute path"
            )
        resolved_value = Path(value).resolve()
        try:
            resolved_value.relative_to(resolved_root)
        except ValueError:
            raise ArenaIsolationError(
                f"arena isolation violated: {key}={value} resolves outside arena root {resolved_root}"
            ) from None


def real_vendor_root(vendor: str, base_env: Mapping[str, str] | None = None) -> Path:
    """The operator's REAL vendor config directory (never a scratch one).

    `$CLAUDE_CONFIG_DIR` / `$CODEX_HOME` if set in `base_env` (the real
    process environment when omitted — note this is deliberately NOT the
    arena `env` a caller builds with `make_arena()`, which has already
    overwritten those same names with scratch paths), else the vendor's
    documented default under the operator's real home.
    """
    if vendor not in VENDOR_STAGED_SURFACES:
        raise ValueError(f"unknown vendor: {vendor}")
    source = base_env if base_env is not None else os.environ
    override = source.get(VENDOR_SOURCE_ENV_VAR[vendor])
    if override:
        return Path(override).resolve()
    return Path.home() / VENDOR_SOURCE_DEFAULT[vendor]


def stage_vendor_config(
    env: Mapping[str, str],
    vendor: str,
    *,
    surface: str = "skill",
    base_env: Mapping[str, str] | None = None,
    repo_root: Path | None = None,
    required_agents: tuple[str, ...] = (),
) -> None:
    """Stage `vendor`'s auth/settings from the real install and, when
    `repo_root` is given, the Planar surface from that checkout.

    Call after `make_arena()` (and, per the fail-closed contract, before
    any vendor host process starts — every call site pairs this with
    `assert_isolated()` immediately before it). `env` is the SCRATCH
    environment `make_arena()` returned; `env[VENDOR_ARENA_ENV_VAR[vendor]]`
    (`CLAUDE_CONFIG_DIR` or `CODEX_HOME`) is the staging target.

    `surface` is the eval surface being run (`"skill"` or `"agent"`); it is
    validated and otherwise unused here. Which agents a run needs is
    stated by `required_agents` (e.g. `("planar-orchestrator",)`).

    For each surface in `CLAUDE_STAGED_SURFACES` / `CODEX_STAGED_SURFACES`:
    a missing OPTIONAL surface is skipped silently; a missing REQUIRED one
    raises `VendorStagingError` naming the absolute path that was expected,
    BEFORE the host starts. An existing target is left alone.

    Those surfaces are SYMLINKED, never copied: a host write to a path that
    does not already exist creates a real file under the scratch directory,
    while a refreshed token written to an existing `auth.json` /
    `.credentials.json` intentionally lands on the real credential file.
    `plugins/` is excluded outright because both vendors rewrite it on
    ordinary session start; see `CLAUDE_STAGED_SURFACES`.

    With `repo_root`, `stage_planar_surface()` then copies the checkout's
    Planar skill and agents into the arena.
    """
    if vendor not in VENDOR_STAGED_SURFACES:
        raise ValueError(f"unknown vendor: {vendor}")
    if surface not in ("skill", "agent"):
        raise ValueError(f"unknown surface: {surface}")
    real_root = real_vendor_root(vendor, base_env)
    scratch_root = Path(env[VENDOR_ARENA_ENV_VAR[vendor]])
    for name, required in VENDOR_STAGED_SURFACES[vendor].items():
        source = real_root / name
        if not source.exists():
            if required:
                raise VendorStagingError(
                    f"{vendor}: required read surface is missing: {source}"
                )
            continue
        target = scratch_root / name
        if target.exists() or target.is_symlink():
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        target.symlink_to(source, target_is_directory=source.is_dir())
    if repo_root is not None:
        stage_planar_surface(env, vendor, repo_root, required_agents=required_agents)


def _frontmatter_description(text: str) -> tuple[str, str]:
    """Split an agent markdown file into (description, body).

    The frontmatter is the leading `---` block; only its `description:`
    line is read (a JSON- or bare-string scalar). Absent frontmatter yields
    an empty description and the whole text as the body.
    """
    if not text.startswith("---\n"):
        return "", text
    end = text.find("\n---", 4)
    if end < 0:
        return "", text
    description = ""
    for line in text[4:end].splitlines():
        if line.startswith("description:"):
            value = line[len("description:"):].strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1]
            description = value
    return description, text[end + 4:].lstrip("\n")


def _toml_basic_string(value: str) -> str:
    return json.dumps(value)


def stage_planar_surface(
    env: Mapping[str, str],
    vendor: str,
    repo_root: Path,
    *,
    required_agents: tuple[str, ...] = (),
) -> None:
    """Copy the checkout's Planar skill and agents into the arena.

    Layout under the arena (`home` is `env["HOME"]`):

    - `<home>/.agents/skills/planar/` — a copy of `<repo_root>/skills/planar/`
      (SKILL.md plus `references/`), the cross-vendor skill location;
    - Claude: `<CLAUDE_CONFIG_DIR>/skills/planar/` (same copy) and
      `<CLAUDE_CONFIG_DIR>/agents/planar-<role>.md` for each
      `<repo_root>/agents/planar-*.md`;
    - Codex: `<CODEX_HOME>/agents/planar-<role>.toml` with `name`,
      `description` and `developer_instructions`, generated from the same
      markdown files;
    - `<home>/.claude` and `<home>/.codex` are symlinks to the scratch
      config directories so both spellings resolve inside the arena.

    No `commands/` directory is ever created. Raises `VendorStagingError`
    naming the expected path when `skills/planar/SKILL.md` or any of
    `required_agents` is absent from `repo_root`.
    """
    if vendor not in VENDOR_STAGED_SURFACES:
        raise ValueError(f"unknown vendor: {vendor}")
    skill_src = repo_root / "skills" / "planar"
    if not (skill_src / "SKILL.md").is_file():
        raise VendorStagingError(
            f"{vendor}: required Planar skill is missing: {skill_src / 'SKILL.md'}"
        )
    agents_src = repo_root / "agents"
    for name in required_agents:
        if not (agents_src / f"{name}.md").is_file():
            raise VendorStagingError(
                f"{vendor}: required Planar agent is missing: {agents_src / (name + '.md')}"
            )
    home = Path(env["HOME"])
    config_dir = Path(env[VENDOR_ARENA_ENV_VAR[vendor]])
    alias = home / (".claude" if vendor == "claude" else ".codex")
    if not (alias.exists() or alias.is_symlink()):
        alias.symlink_to(config_dir, target_is_directory=True)

    skill_targets = [home / ".agents" / "skills" / "planar"]
    if vendor == "claude":
        skill_targets.append(config_dir / "skills" / "planar")
    for target in skill_targets:
        if target.exists():
            shutil.rmtree(target)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(skill_src, target)

    agent_dir = config_dir / "agents"
    for source in sorted(agents_src.glob("planar-*.md")):
        agent_dir.mkdir(parents=True, exist_ok=True)
        if vendor == "claude":
            shutil.copy2(source, agent_dir / source.name)
            continue
        text = source.read_text(encoding="utf-8")
        description, body = _frontmatter_description(text)
        name = source.stem
        (agent_dir / f"{name}.toml").write_text(
            f"name = {_toml_basic_string(name)}\n"
            f"description = {_toml_basic_string(description)}\n"
            f"developer_instructions = {_toml_basic_string(body)}\n",
            encoding="utf-8",
        )


def assert_vendor_auth(env: Mapping[str, str], vendor: str) -> None:
    """Fail closed unless `vendor` has a usable auth surface in `env`.

    Call AFTER `stage_vendor_config()` (so a staged `auth.json` is in
    place to check for codex) and, per the fail-closed contract, before
    any vendor host process starts — every live/lifecycle call site pairs
    this with `assert_isolated()` and `stage_vendor_config()` immediately
    before it: `make_arena()` -> `assert_isolated()` ->
    `stage_vendor_config()` -> `assert_vendor_auth()`.

    Keychain-backed `claude` login does not follow a scratch
    `CLAUDE_CONFIG_DIR` (Planar artifact 626 / task 6872: a live run
    staged `agents/`/`commands/` correctly and still answered "Not logged
    in · Please run /login"). So arena Claude auth must be an explicit
    env token:

    - claude: `env["CLAUDE_CODE_OAUTH_TOKEN"]` (minted by
      `claude setup-token`) or `env["ANTHROPIC_API_KEY"]` must be a
      non-empty value.
    - codex: `$CODEX_HOME/auth.json` (staged by `stage_vendor_config()`,
      or provided by the caller) must exist, or `env["OPENAI_API_KEY"]`
      must be a non-empty value.

    Raises `VendorAuthError` naming exactly which variable or file is
    missing and how to obtain it. Never includes a credential VALUE in
    the message, even when one is present in `env` — only presence is
    checked, never logged.
    """
    if vendor not in VENDOR_STAGED_SURFACES:
        raise ValueError(f"unknown vendor: {vendor}")
    if vendor == "claude":
        if env.get("CLAUDE_CODE_OAUTH_TOKEN") or env.get("ANTHROPIC_API_KEY"):
            return
        raise VendorAuthError(
            "claude: no usable auth in the arena environment. Set "
            "CLAUDE_CODE_OAUTH_TOKEN (run `claude setup-token`) or "
            "ANTHROPIC_API_KEY before running a live/lifecycle eval — "
            "Keychain-backed `claude login` does not follow a scratch "
            "CLAUDE_CONFIG_DIR."
        )
    # vendor == "codex"
    if env.get("OPENAI_API_KEY"):
        return
    codex_home = env.get("CODEX_HOME")
    if codex_home and (Path(codex_home) / "auth.json").exists():
        return
    raise VendorAuthError(
        "codex: no usable auth in the arena environment. Stage "
        "$CODEX_HOME/auth.json (via stage_vendor_config, from the real "
        "install) or set OPENAI_API_KEY before running a live/lifecycle "
        "eval."
    )
