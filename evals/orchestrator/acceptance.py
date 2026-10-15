#!/usr/bin/env python3
"""Three-orchestrator acceptance eval: three orchestrators conform on one plan.

The acceptance scenario of the mechanism-only plan: on one synthetic plan with a
parallel wave and a milestone gate, three different orchestrators each finish
the plan with zero `planar health conformance` violations, with no methodology
installed. The three are:

- ``claude-stock``: ``claude -p`` told only ``use planar for plan <id>``;
- ``claude-swarm``: ``claude -p`` given the swarm prompt from
  ``examples/swarm-orchestration.md`` with its placeholders filled;
- ``codex-stock``: ``codex exec`` told only ``use planar for plan <id>``.

Each run gets a fresh *sandbox*, built from nothing:

1. A release bundle is assembled from the checkout's built binaries
   (``build/debug/bin``) the way ``scripts/dist.sh`` lays one out, and the real
   installer runs over it (``install.sh --prebuilt``) into a scratch ``HOME``.
   The install therefore carries exactly what an operator's install carries: the
   binaries, the ``planar`` skill placed into the scratch ``~/.claude/skills``
   and ``~/.agents/skills``, and no role agents or methodology.
2. A small git repository and a fixture plan are created with the installed
   binaries against the sandbox's own database: an anchor plan, two milestone
   child plans, three disjoint tasks in the first milestone (the parallel wave)
   and one task in the second that depends on all three (the milestone gate).
   The scope's policy sets ``review_required``, ``use_queue`` and a
   ``task_gate``. The host queue lives in the sandbox database, so it is the
   sandbox's own queue.
3. The orchestrator runs headless with ``HOME``, ``PLANAR_HOME``, ``PLANAR_DB``
   and every other Planar path inside the sandbox and the repository as its
   working directory, under a wall-clock limit.
4. The trail it leaves is graded by ``trail.grade_database`` (``planar health
   conformance <anchor> --json``, from the installed branch build, over a copy
   of the sandbox database). A run passes only when the checker reports zero
   violations at exit 0, it graded at least one claim, and every fixture task is
   ``done``. A report that cannot be read is a failure, never a pass.

Planar's binaries never spawn a model; this eval harness does, and it is
operator-run. Credentials are never copied from the operator's home: the only
auth a run sees is an explicit environment variable from ``AUTH_ENV_VARS``. A
vendor with none of them, or whose ``say ok`` probe fails, records the run as
``blocked``.

Usage::

    python3 evals/orchestrator/acceptance.py [--runs claude-stock,claude-swarm,codex-stock]
        [--timeout-minutes 40] [--results DIR] [--keep] [--no-ledger]
    python3 evals/orchestrator/acceptance.py --fixture-only [--keep]

Standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace
from typing import Any, Callable, Mapping, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))

import trail  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
CASE_ID = "three-orchestrators"
LEDGER_MODE = "acceptance"
SWARM_EXAMPLE = ROOT / "examples" / "swarm-orchestration.md"
DEFAULT_BIN_DIR = ROOT / "build" / "debug" / "bin"
DEFAULT_RESULTS_PARENT = ROOT / "build" / "eval-acceptance"
PLANAR_BINARIES = ("planar", "planar-agent", "planar-watch", "planar-ext")
DEFAULT_TIMEOUT_MINUTES = 40
AUTH_PROBE_SECONDS = 180

# The only variables a run inherits from the harness's environment: an explicit
# credential per vendor, and locale. Nothing else crosses, PATH included.
AUTH_ENV_VARS: dict[str, tuple[str, ...]] = {
    "claude": ("ANTHROPIC_API_KEY", "CLAUDE_CODE_OAUTH_TOKEN"),
    "codex": ("OPENAI_API_KEY", "CODEX_API_KEY"),
}
LOCALE_VARS = ("LANG", "LC_ALL", "LC_CTYPE")

# System directories a run's PATH may hold after the sandbox's own `bin` and
# vendor shims. Any that resolves under the operator's real home is dropped.
SYSTEM_PATH = ("/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin", "/usr/sbin", "/sbin")

# Every variable that names a Planar or vendor state path. Each must resolve
# inside the sandbox root.
SANDBOX_PATH_VARS = (
    "HOME",
    "PLANAR_HOME",
    "PLANAR_DB",
    "PLANAR_CONFIG_PATH",
    "PLANAR_LOCAL_HOME",
    "PLANAR_WORKBENCH_ROOT",
    "CLAUDE_CONFIG_DIR",
    "CODEX_HOME",
    "TMPDIR",
)

# Instruction files a host reads from the working directory's ancestors. A
# sandbox under any of them would hand the run instructions it must not have.
ANCESTOR_INSTRUCTION_FILES = ("CLAUDE.md", "AGENTS.md", "CLAUDE.local.md")

POLICY: dict[str, Any] = {
    "review_required": True,
    "use_queue": True,
    "task_gate": ["sh", "check.sh"],
}


class AcceptanceError(RuntimeError):
    """The sandbox could not be built, a run could not start, or a grade is unreadable."""


# --- the fixture ------------------------------------------------------------


@dataclass(frozen=True)
class TaskSpec:
    """One fixture task: the module and test it adds."""

    milestone: int
    slug: str
    title: str
    path: str
    test_path: str
    body: str
    depends: tuple[str, ...] = ()


ANCHOR_TITLE = "Calculator operations"
ANCHOR_SLUG = "acc-calc"
MILESTONES = ("Operations", "Dispatcher")
FIXTURE_TASKS: tuple[TaskSpec, ...] = (
    TaskSpec(
        0,
        "acc-calc-add",
        "Add the add operation",
        "calc/add.py",
        "tests/test_add.py",
        "Create calc/add.py with `def add(a, b)` returning a + b, and tests/test_add.py "
        "with a unittest case covering two positive numbers and a negative one. "
        "Touch no other file. Acceptance: `sh check.sh` passes.",
    ),
    TaskSpec(
        0,
        "acc-calc-mul",
        "Add the multiply operation",
        "calc/mul.py",
        "tests/test_mul.py",
        "Create calc/mul.py with `def mul(a, b)` returning a * b, and tests/test_mul.py "
        "with a unittest case covering zero, positive and negative factors. "
        "Touch no other file. Acceptance: `sh check.sh` passes.",
    ),
    TaskSpec(
        0,
        "acc-calc-neg",
        "Add the negate operation",
        "calc/neg.py",
        "tests/test_neg.py",
        "Create calc/neg.py with `def neg(a)` returning -a, and tests/test_neg.py "
        "with a unittest case covering zero, a positive and a negative number. "
        "Touch no other file. Acceptance: `sh check.sh` passes.",
    ),
    TaskSpec(
        1,
        "acc-calc-dispatch",
        "Add the operation dispatcher",
        "calc/dispatch.py",
        "tests/test_dispatch.py",
        "Create calc/dispatch.py with `def evaluate(op, *args)` that maps 'add', 'mul' "
        "and 'neg' to calc.add.add, calc.mul.mul and calc.neg.neg and raises "
        "ValueError for any other op, and tests/test_dispatch.py covering each op "
        "and the error. Touch no other file. Acceptance: `sh check.sh` passes.",
        depends=("acc-calc-add", "acc-calc-mul", "acc-calc-neg"),
    ),
)

REPO_FILES: dict[str, str] = {
    "README.md": "# calc\n\nA tiny calculator package used as an orchestration fixture.\n",
    "calc/__init__.py": '"""A tiny calculator package."""\n',
    "tests/__init__.py": "",
    "tests/test_package.py": (
        "import unittest\n\nimport calc\n\n\n"
        "class PackageTest(unittest.TestCase):\n"
        "    def test_imports(self):\n"
        "        self.assertTrue(calc.__doc__)\n\n\n"
        'if __name__ == "__main__":\n    unittest.main()\n'
    ),
    "check.sh": "#!/bin/sh\n# The task gate: every unit test in tests/.\nset -e\nexec python3 -m unittest discover -s tests -t .\n",
}


PROJECT_SLUG = "calc"
ASSOCIATION = f"project:{PROJECT_SLUG}"


def policy_toml(scope: str = ASSOCIATION, policy: Mapping[str, Any] = POLICY) -> str:
    """The sandbox `config.toml`: the policy as the fixture association's table."""
    lines = [f'[associations."{scope}".policy]']
    for key, value in policy.items():
        if isinstance(value, bool):
            lines.append(f"{key} = {'true' if value else 'false'}")
        elif isinstance(value, list) and all(isinstance(item, str) for item in value):
            lines.append(f"{key} = {json.dumps(value)}")
        else:
            raise AcceptanceError(f"policy {key!r} must be a boolean or a list of strings")
    return "\n".join(lines) + "\n"


def write_repo(repo: Path) -> None:
    """Create the fixture git repository at `repo` with one commit on `main`."""
    repo.mkdir(parents=True, exist_ok=True)
    for relative, text in REPO_FILES.items():
        target = repo / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")
    (repo / "check.sh").chmod(0o755)
    git_env = {"PATH": os.pathsep.join(SYSTEM_PATH), "HOME": str(repo.parent), "GIT_CONFIG_NOSYSTEM": "1"}

    def git(*argv: str) -> None:
        result = subprocess.run(
            ["git", *argv], cwd=repo, env=git_env, capture_output=True, text=True, check=False
        )
        if result.returncode != 0:
            raise AcceptanceError(f"git {' '.join(argv)} failed: {result.stderr.strip()}")

    git("init", "-q", "-b", "main")
    git("config", "user.name", "Acceptance Fixture")
    git("config", "user.email", "acceptance@example.invalid")
    git("add", "-A")
    git("commit", "-q", "-s", "-m", "Start the calc fixture")


@dataclass
class Fixture:
    """A built sandbox: the install, the repository and the fixture plan's ids."""

    root: Path
    env: dict[str, str]
    repo: Path
    scope: str = ""
    anchor_id: int = 0
    milestone_ids: list[int] = field(default_factory=list)
    task_ids: dict[str, int] = field(default_factory=dict)

    @property
    def home(self) -> Path:
        return Path(self.env["HOME"])

    @property
    def planar_home(self) -> Path:
        return Path(self.env["PLANAR_HOME"])

    @property
    def database(self) -> Path:
        return Path(self.env["PLANAR_DB"])

    @property
    def config(self) -> Path:
        return Path(self.env["PLANAR_CONFIG_PATH"])

    @property
    def bin_dir(self) -> Path:
        return self.planar_home / "bin"

    def describe(self) -> dict[str, Any]:
        return {
            "root": str(self.root),
            "repo": str(self.repo),
            "scope": self.scope,
            "anchor_id": self.anchor_id,
            "milestone_ids": self.milestone_ids,
            "task_ids": self.task_ids,
        }


# A runner takes an argv (the binary name first) and returns parsed stdout.
Runner = Callable[[Sequence[str]], str]


def _json(text: str, what: str) -> Any:
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        raise AcceptanceError(f"{what}: output is not JSON: {text[:200]!r}") from exc


def _id_of(value: Any, what: str) -> int:
    if isinstance(value, dict):
        for key in ("id", "plan_id", "task_id"):
            if isinstance(value.get(key), int):
                return value[key]
        for nested in ("plan", "task", "project"):
            if isinstance(value.get(nested), dict) and isinstance(value[nested].get("id"), int):
                return value[nested]["id"]
    raise AcceptanceError(f"{what}: no id in {value!r}")


def create_fixture_plan(fixture: Fixture, run: Runner) -> None:
    """Register the repository and create the fixture plan through the CLIs.

    Every write goes through `run` (the installed binaries in the sandbox
    environment, cwd the repository), so the plan is made the way an operator
    makes one and nothing is written to the database directly.
    """
    project = _json(run(["planar", "init", "--name", PROJECT_SLUG, "--slug", PROJECT_SLUG, "--json"]), "planar init")
    if project.get("project_slug") != PROJECT_SLUG:
        raise AcceptanceError(f"planar init registered {project.get('project_slug')!r}, not {PROJECT_SLUG!r}")
    run(["planar", "assoc", "create", ASSOCIATION, "--kind", "project"])
    run(["planar", "assoc", "add", ASSOCIATION, str(fixture.repo)])
    fixture.scope = ASSOCIATION
    fixture.config.write_text(policy_toml(ASSOCIATION), encoding="utf-8")
    run(["planar", "config", "validate"])
    anchor = _json(
        run(["planar", "plan", "create", ANCHOR_TITLE, "--slug", ANCHOR_SLUG, "--status", "active", "--json"]),
        "plan create (anchor)",
    )
    fixture.anchor_id = _id_of(anchor, "plan create (anchor)")
    fixture.milestone_ids = []
    for index, title in enumerate(MILESTONES):
        milestone = _json(
            run(
                [
                    "planar", "plan", "create", f"M{index}: {title}",
                    "--slug", f"{ANCHOR_SLUG}-m{index}",
                    "--parent", str(fixture.anchor_id),
                    "--status", "active", "--json",
                ]
            ),
            f"plan create (milestone {index})",
        )
        fixture.milestone_ids.append(_id_of(milestone, f"plan create (milestone {index})"))
    fixture.task_ids = {}
    for spec in FIXTURE_TASKS:
        created = _json(
            run(
                [
                    "planar", "task", "add", spec.title,
                    "--plan", str(fixture.milestone_ids[spec.milestone]),
                    "--slug", spec.slug,
                    "--body", spec.body,
                    "--next-action", f"Create {spec.path} and {spec.test_path}",
                    "--editor=false", "--json",
                ]
            ),
            f"task add {spec.slug}",
        )
        task_id = _id_of(created, f"task add {spec.slug}")
        fixture.task_ids[spec.slug] = task_id
        for path in (spec.path, spec.test_path):
            run(["planar", "task", "touches", "add", str(task_id), PROJECT_SLUG, "--path", path])
    for spec in FIXTURE_TASKS:
        for dependency in spec.depends:
            run(
                [
                    "planar", "task", "link", str(fixture.task_ids[spec.slug]),
                    f"task:{fixture.task_ids[dependency]}", "--relationship", "depends-on",
                ]
            )


def check_readiness(fixture: Fixture, run: Runner) -> dict[str, list[str]]:
    """Check every fixture task's packet: the wave is ready, the gate task waits.

    Returns each slug's readiness reasons. The first milestone's tasks must be
    `ready: true`; the second milestone's task must be held only by
    `blocked_by_open_task`, which is the milestone gate.
    """
    reasons: dict[str, list[str]] = {}
    for spec in FIXTURE_TASKS:
        packet = _json(
            run(["planar", "task", "packet", str(fixture.task_ids[spec.slug]), "--json"]),
            f"task packet {spec.slug}",
        )
        found = [str(r.get("code", r)) if isinstance(r, dict) else str(r) for r in packet.get("reasons", [])]
        reasons[spec.slug] = found
        if spec.depends:
            if packet.get("ready") is not False or found != ["blocked_by_open_task"]:
                raise AcceptanceError(
                    f"{spec.slug} should wait only on its dependencies, got ready={packet.get('ready')} {found}"
                )
        elif packet.get("ready") is not True:
            raise AcceptanceError(f"{spec.slug} is not ready: {found}")
    return reasons


# --- the sandbox environment -----------------------------------------------


def real_home(base: Mapping[str, str] | None = None) -> Path:
    """The operator's real home, which nothing in a run may point into."""
    source = os.environ if base is None else base
    return Path(source.get("HOME") or Path.home()).resolve()


def _inside(path: Path, root: Path) -> bool:
    return path.resolve().is_relative_to(root.resolve())


def system_path_entries(operator_home: Path) -> list[str]:
    """The `SYSTEM_PATH` directories that exist and are not under `operator_home`."""
    return [entry for entry in SYSTEM_PATH if Path(entry).is_dir() and not _inside(Path(entry), operator_home)]


def sandbox_env(
    root: Path,
    *,
    base: Mapping[str, str] | None = None,
    vendor: str | None = None,
    operator_home: Path | None = None,
) -> dict[str, str]:
    """The complete environment for every child of a sandbox rooted at `root`.

    Built from nothing: the Planar and vendor paths all under `root`, a PATH of
    the sandbox's installed `bin`, its vendor shims and the system directories,
    locale, and for `vendor` only that vendor's explicit credential variables
    from `base`. Nothing else from `base` crosses.
    """
    source = os.environ if base is None else base
    home_of_operator = operator_home or real_home(source)
    root = root.resolve()
    home = root / "home"
    planar_home = home / ".planar"
    env = {
        "HOME": str(home),
        "PLANAR_HOME": str(planar_home),
        "PLANAR_DB": str(planar_home / "planar.db"),
        "PLANAR_CONFIG_PATH": str(planar_home / "config.toml"),
        "PLANAR_LOCAL_HOME": str(planar_home / "local"),
        "PLANAR_WORKBENCH_ROOT": str(planar_home / "workbench"),
        "CLAUDE_CONFIG_DIR": str(home / ".claude"),
        "CODEX_HOME": str(home / ".codex"),
        "TMPDIR": str(root / "tmp"),
        "PATH": os.pathsep.join(
            [str(planar_home / "bin"), str(root / "vendor-bin"), *system_path_entries(home_of_operator)]
        ),
        "NO_COLOR": "1",
        "GIT_CONFIG_NOSYSTEM": "1",
    }
    for name in LOCALE_VARS:
        if source.get(name):
            env[name] = source[name]
    if vendor is not None:
        for name in AUTH_ENV_VARS[vendor]:
            if source.get(name):
                env[name] = source[name]
    return env


def assert_sandboxed(env: Mapping[str, str], root: Path, operator_home: Path | None = None) -> None:
    """Fail closed unless `env` cannot reach the operator's Planar state.

    Every `SANDBOX_PATH_VARS` value must be absolute and inside `root`; no PATH
    entry, and no other value, may lie under the operator's home (and so under
    `~/.planar`); and `root` itself must not be under the operator's home.
    """
    home_of_operator = (operator_home or real_home()).resolve()
    root = root.resolve()
    if _inside(root, home_of_operator):
        raise AcceptanceError(f"the sandbox {root} lies under the operator's home {home_of_operator}")
    for name in SANDBOX_PATH_VARS:
        value = env.get(name)
        if not value or not Path(value).is_absolute():
            raise AcceptanceError(f"{name} must be an absolute path in the sandbox, got {value!r}")
        if not _inside(Path(value), root):
            raise AcceptanceError(f"{name}={value} lies outside the sandbox {root}")
    for entry in env.get("PATH", "").split(os.pathsep):
        if not entry or not Path(entry).is_absolute():
            raise AcceptanceError(f"PATH holds a relative or empty entry: {entry!r}")
        if _inside(Path(entry), home_of_operator):
            raise AcceptanceError(f"PATH entry {entry} lies under the operator's home")
    planar_state = home_of_operator / ".planar"
    for name, value in env.items():
        if name in AUTH_ENV_VARS["claude"] + AUTH_ENV_VARS["codex"]:
            continue
        for piece in value.split(os.pathsep):
            if piece.startswith("/") and (_inside(Path(piece), home_of_operator) or _inside(Path(piece), planar_state)):
                raise AcceptanceError(f"{name} reaches into the operator's home: {piece}")


def assert_no_ancestor_instructions(directory: Path) -> None:
    """Refuse a sandbox under a directory holding a host instruction file.

    Claude reads `CLAUDE.md` from every ancestor of its working directory, so a
    sandbox inside a checkout would hand the run that checkout's instructions.
    """
    for ancestor in [directory.resolve(), *directory.resolve().parents]:
        for name in ANCESTOR_INSTRUCTION_FILES:
            if (ancestor / name).exists():
                raise AcceptanceError(f"the sandbox {directory} is under {ancestor / name}; choose another --sandbox-root")


def make_vendor_shims(root: Path, binaries: Mapping[str, Path]) -> None:
    """Link each vendor CLI into `root/vendor-bin` so PATH never names the operator's home."""
    shim_dir = root / "vendor-bin"
    shim_dir.mkdir(parents=True, exist_ok=True)
    for name, target in binaries.items():
        link = shim_dir / name
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(target)


# --- the install ------------------------------------------------------------


def migration_head(repo_root: Path = ROOT) -> int:
    """The highest numbered migration in the checkout."""
    numbers = [
        int(match.group(1))
        for path in (repo_root / "migrations").glob("*.up.sql")
        if (match := re.match(r"(\d+)_", path.name))
    ]
    if not numbers:
        raise AcceptanceError("no migrations found")
    return max(numbers)


def source_sha(repo_root: Path = ROOT) -> str:
    """The checkout's HEAD sha, or forty zeros outside git."""
    result = subprocess.run(
        ["git", "-C", str(repo_root), "rev-parse", "HEAD"], capture_output=True, text=True, check=False
    )
    sha = result.stdout.strip()
    return sha if re.fullmatch(r"[0-9a-f]{40}", sha) else "0" * 40


def assemble_bundle(dest: Path, *, bin_dir: Path = DEFAULT_BIN_DIR, repo_root: Path = ROOT) -> Path:
    """Lay out a release bundle at `dest` from the built binaries in `bin_dir`.

    The layout is `scripts/dist.sh`'s: the installer and uninstaller, the
    cleanup list, the four binaries, the `planar` skill, templates, migrations,
    `scripts/install-lib` and a `release.json`. No `agents/` or `codex-agents/`
    is staged, because a release bundle carries none.
    """
    missing = [name for name in PLANAR_BINARIES if not (bin_dir / name).is_file()]
    if missing:
        raise AcceptanceError(f"{bin_dir} lacks {', '.join(missing)}; build the checkout first")
    ignore = shutil.ignore_patterns("__pycache__", "*.pyc", ".DS_Store")
    if dest.exists():
        shutil.rmtree(dest)
    (dest / "bin").mkdir(parents=True)
    (dest / "scripts").mkdir()
    (dest / "skills").mkdir()
    for name in PLANAR_BINARIES:
        # A hard link where the filesystem allows one: debug binaries are large,
        # the installer only reads the bundle, and `build_sandbox` removes the
        # bundle once the install is done.
        try:
            os.link(bin_dir / name, dest / "bin" / name)
        except OSError:
            shutil.copy2(bin_dir / name, dest / "bin" / name)
    shutil.copytree(repo_root / "skills" / "planar", dest / "skills" / "planar", symlinks=True, ignore=ignore)
    shutil.copytree(repo_root / "scripts" / "install-lib", dest / "scripts" / "install-lib", symlinks=True, ignore=ignore)
    for tree in ("templates", "migrations"):
        shutil.copytree(repo_root / tree, dest / tree, symlinks=True, ignore=ignore)
    shutil.copy2(repo_root / "install.sh", dest / "install.sh")
    shutil.copy2(repo_root / "install-cleanup.txt", dest / "install-cleanup.txt")
    shutil.copy2(repo_root / "scripts" / "uninstall.sh", dest / "uninstall.sh")
    shutil.copy2(repo_root / "scripts" / "get-planar.sh", dest / "get-planar.sh")
    release = {
        "version": "dev",
        "sha": source_sha(repo_root),
        "date": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "os": "macos" if sys.platform == "darwin" else "linux",
        "arch": "arm64" if os.uname().machine in ("arm64", "aarch64") else "x86_64",
        "os_floor": "26.0" if sys.platform == "darwin" else "2.36",
        "schema_version": migration_head(repo_root),
    }
    (dest / "release.json").write_text(json.dumps(release, indent=2) + "\n", encoding="utf-8")
    return dest


def install_bundle(bundle: Path, env: Mapping[str, str], log: Path) -> None:
    """Run the bundle's installer in prebuilt mode into the sandbox `HOME`."""
    home = Path(env["HOME"])
    for vendor_dir in (".claude", ".codex"):
        (home / vendor_dir).mkdir(parents=True, exist_ok=True)
    Path(env["TMPDIR"]).mkdir(parents=True, exist_ok=True)
    installer_env = {key: env[key] for key in ("HOME", "PLANAR_HOME", "PLANAR_DB", "TMPDIR", "CODEX_HOME")}
    installer_env.update({"PATH": os.pathsep.join(SYSTEM_PATH), "NO_COLOR": "1", "LC_ALL": "C"})
    result = subprocess.run(
        ["/bin/bash", str(bundle / "install.sh"), "--prebuilt", str(bundle)],
        cwd=home,
        env=installer_env,
        capture_output=True,
        text=True,
        timeout=600,
        check=False,
    )
    log.write_text(result.stdout + "\n--- stderr ---\n" + result.stderr, encoding="utf-8")
    if result.returncode != 0:
        raise AcceptanceError(f"install.sh --prebuilt failed ({result.returncode}); see {log}")


def check_install(env: Mapping[str, str]) -> None:
    """The install placed the skill for both vendors and no methodology at all."""
    home = Path(env["HOME"])
    planar_home = Path(env["PLANAR_HOME"])
    for name in PLANAR_BINARIES:
        if not os.access(planar_home / "bin" / name, os.X_OK):
            raise AcceptanceError(f"the install has no executable bin/{name}")
    for skill in (home / ".claude" / "skills" / "planar", home / ".agents" / "skills" / "planar"):
        if not (skill / "SKILL.md").is_file():
            raise AcceptanceError(f"the install did not place the planar skill at {skill}")
    for absent in (
        planar_home / "agents",
        planar_home / "codex-agents",
        home / ".claude" / "agents",
        home / ".codex" / "agents",
    ):
        if absent.exists():
            raise AcceptanceError(f"the install placed methodology at {absent}")
    if not Path(env["PLANAR_DB"]).is_file():
        raise AcceptanceError("the install did not create the sandbox database")


def make_runner(env: Mapping[str, str], cwd: Path, log: Path) -> Runner:
    """A runner for the installed binaries: refuses any binary outside the sandbox `bin`."""
    bin_dir = Path(env["PLANAR_HOME"]) / "bin"

    def run(argv: Sequence[str]) -> str:
        binary = bin_dir / argv[0]
        if argv[0] not in PLANAR_BINARIES or not binary.is_file():
            raise AcceptanceError(f"not an installed Planar binary: {argv[0]}")
        result = subprocess.run(
            [str(binary), *argv[1:]], cwd=cwd, env=dict(env), capture_output=True, text=True, timeout=120, check=False
        )
        with log.open("a", encoding="utf-8") as handle:
            handle.write(f"$ {' '.join(argv)}\n[exit {result.returncode}]\n{result.stdout}{result.stderr}\n")
        if result.returncode != 0:
            raise AcceptanceError(f"{' '.join(argv)} exited {result.returncode}: {result.stderr.strip() or result.stdout.strip()}")
        return result.stdout

    return run


def build_sandbox(
    root: Path,
    *,
    bin_dir: Path = DEFAULT_BIN_DIR,
    vendor: str | None = None,
    vendor_binaries: Mapping[str, Path] | None = None,
    base: Mapping[str, str] | None = None,
) -> Fixture:
    """Build one complete sandbox at `root`: bundle, install, repository, plan."""
    root.mkdir(parents=True, exist_ok=True)
    root = root.resolve()
    assert_no_ancestor_instructions(root)
    env = sandbox_env(root, base=base, vendor=vendor)
    assert_sandboxed(env, root)
    for name in ("TMPDIR",):
        Path(env[name]).mkdir(parents=True, exist_ok=True)
    make_vendor_shims(root, vendor_binaries or {})
    bundle = assemble_bundle(root / "bundle", bin_dir=bin_dir)
    try:
        install_bundle(bundle, env, root / "install.log")
    finally:
        shutil.rmtree(bundle, ignore_errors=True)
    check_install(env)
    repo = root / "repo"
    write_repo(repo)
    fixture = Fixture(root=root, env=env, repo=repo)
    runner = make_runner(env, repo, root / "fixture.log")
    create_fixture_plan(fixture, runner)
    check_readiness(fixture, runner)
    (root / "fixture.json").write_text(json.dumps(fixture.describe(), indent=2) + "\n", encoding="utf-8")
    return fixture


# --- the three runs ---------------------------------------------------------


@dataclass(frozen=True)
class RunSpec:
    """One orchestrator under test."""

    name: str
    vendor: str
    surface: str


RUNS: dict[str, RunSpec] = {
    "claude-stock": RunSpec("claude-stock", "claude", "stock"),
    "claude-swarm": RunSpec("claude-swarm", "claude", "swarm"),
    "codex-stock": RunSpec("codex-stock", "codex", "stock"),
}


def stock_prompt(fixture: Fixture) -> str:
    """The whole instruction a stock session gets."""
    return f"use planar for plan {fixture.anchor_id}"


def swarm_template(example: Path = SWARM_EXAMPLE) -> str:
    """The fenced orchestrator prompt under `## The orchestrator prompt`."""
    text = example.read_text(encoding="utf-8")
    heading = text.find("## The orchestrator prompt")
    if heading < 0:
        raise AcceptanceError(f"{example} has no '## The orchestrator prompt' section")
    match = re.search(r"```text\n(.*?)\n```", text[heading:], re.DOTALL)
    if not match:
        raise AcceptanceError(f"{example} has no fenced text prompt under its orchestrator section")
    return match.group(1)


def swarm_prompt(fixture: Fixture, *, worker_model: str = "haiku", writer_model: str = "sonnet") -> str:
    """The swarm example's prompt with every placeholder filled for the fixture."""
    values = {
        "<plan-id>": str(fixture.anchor_id),
        "<repo-checkout>": str(fixture.repo),
        "<plan-branch>": "main",
        "<plan-worktree>": str(fixture.repo),
        "<worker-model>": worker_model,
        "<writer-model>": writer_model,
        "<n>": "3",
    }
    prompt = swarm_template()
    for placeholder, value in values.items():
        prompt = prompt.replace(placeholder, value)
    # The merge-policy line's angle brackets mark an editable default.
    prompt = prompt.replace(
        "<orchestrator merges into the plan branch; the\noperator merges the plan branch>",
        "orchestrator merges into the plan branch; the\noperator merges the plan branch",
    )
    # `<id>` and `<sha>` stay: they are command templates the orchestrator fills.
    leftover = [token for token in re.findall(r"<[a-z][a-z-]*>", prompt) if token not in ("<id>", "<sha>")]
    if leftover:
        raise AcceptanceError(f"unfilled swarm placeholders: {sorted(set(leftover))}")
    return prompt


def prompt_for(run: RunSpec, fixture: Fixture) -> str:
    return swarm_prompt(fixture) if run.surface == "swarm" else stock_prompt(fixture)


def build_command(run: RunSpec, fixture: Fixture, prompt: str, model: str | None = None) -> list[str]:
    """The headless argv for `run`.

    Both hosts run unattended, so permission prompts are off; that is safe only
    because every path the run is given is inside the sandbox. Claude may write
    the sandbox root (`--add-dir`); Codex runs in its workspace-write sandbox
    with the sandbox root as an extra writable directory.
    """
    root = str(fixture.root)
    if run.vendor == "claude":
        argv = ["claude", "-p", "--output-format", "stream-json", "--verbose", "--dangerously-skip-permissions", "--add-dir", root]
        if model:
            argv += ["--model", model]
        return [*argv, prompt]
    if run.vendor == "codex":
        argv = ["codex", "exec", "--json", "--sandbox", "workspace-write", "--add-dir", root, "-C", str(fixture.repo)]
        if model:
            argv += ["--model", model]
        return [*argv, prompt]
    raise AcceptanceError(f"unknown vendor {run.vendor!r}")


def probe_command(vendor: str) -> list[str]:
    """A one-line `say ok` that proves the vendor CLI is authenticated in the sandbox."""
    if vendor == "claude":
        return ["claude", "-p", "say ok"]
    return ["codex", "exec", "--skip-git-repo-check", "--sandbox", "read-only", "say ok"]


@dataclass
class HostResult:
    exit_code: int | None
    seconds: float
    timed_out: bool


def run_host(argv: Sequence[str], env: Mapping[str, str], cwd: Path, timeout: float, stdout: Path, stderr: Path) -> HostResult:
    """Run one host process to completion or `timeout`, killing its process group on timeout."""
    started = time.monotonic()
    with stdout.open("wb") as out, stderr.open("wb") as err:
        process = subprocess.Popen(
            list(argv), cwd=cwd, env=dict(env), stdin=subprocess.DEVNULL, stdout=out, stderr=err, start_new_session=True
        )
        try:
            code: int | None = process.wait(timeout=timeout)
            timed_out = False
        except subprocess.TimeoutExpired:
            timed_out = True
            with _suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                with _suppress(ProcessLookupError):
                    os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            code = None
    return HostResult(exit_code=code, seconds=round(time.monotonic() - started, 1), timed_out=timed_out)


class _suppress:
    def __init__(self, *kinds: type[BaseException]):
        self.kinds = kinds

    def __enter__(self) -> None:
        return None

    def __exit__(self, kind, value, tb) -> bool:  # noqa: ANN001
        return kind is not None and issubclass(kind, self.kinds)


def auth_problem(vendor: str, env: Mapping[str, str], cwd: Path, scratch: Path) -> str | None:
    """Why `vendor` cannot run in the sandbox, or None when its `say ok` probe works.

    Presence of an explicit credential variable is checked first; no credential
    is ever read from, linked to or copied out of the operator's home.
    """
    names = AUTH_ENV_VARS[vendor]
    if not any(env.get(name) for name in names):
        return (
            f"{vendor}: no credential in the sandbox environment; set one of {', '.join(names)}. "
            "The operator's own login is never copied into the sandbox."
        )
    scratch.mkdir(parents=True, exist_ok=True)
    result = run_host(probe_command(vendor), env, cwd, AUTH_PROBE_SECONDS, scratch / f"probe-{vendor}.out", scratch / f"probe-{vendor}.err")
    if result.exit_code != 0:
        tail = (scratch / f"probe-{vendor}.out").read_text(errors="replace")[-400:]
        return f"{vendor}: the say-ok probe failed (exit {result.exit_code}, timed out={result.timed_out}): {tail.strip()}"
    return None


# --- grading ----------------------------------------------------------------


@dataclass
class RunGrade:
    """The verdict on one run's trail."""

    grade: str  # pass | fail | blocked
    reasons: list[str]
    report: dict[str, Any] | None
    tasks: dict[str, str]

    def as_json(self) -> dict[str, Any]:
        return {"status": self.grade, "reasons": self.reasons, "tasks": self.tasks, "conformance": self.report}


def task_statuses(database: Path, task_ids: Mapping[str, int]) -> dict[str, str]:
    """Each fixture task's status, read from a read-only snapshot of `database`."""
    with tempfile.TemporaryDirectory(prefix="acceptance-tasks-") as tmp:
        copy = Path(tmp) / "planar.db"
        trail.snapshot_database(database, copy)
        connection = sqlite3.connect(f"file:{copy}?mode=ro", uri=True)
        try:
            statuses: dict[str, str] = {}
            for slug, task_id in task_ids.items():
                row = connection.execute("select status from tasks where id = ?", (task_id,)).fetchone()
                statuses[slug] = row[0] if row else "missing"
            return statuses
        finally:
            connection.close()


def judge(grade: trail.TrailGrade | None, error: str | None, tasks: Mapping[str, str]) -> RunGrade:
    """Turn the checker's report and the task states into a pass or a fail.

    A run passes only when the report was read, it conforms at exit 0 with no
    violations, it graded at least one claim, and every fixture task is done.
    """
    reasons: list[str] = []
    if grade is None:
        reasons.append(f"conformance report unreadable: {error or 'no report'}")
        return RunGrade("fail", reasons, None, dict(tasks))
    if not grade.conforms or grade.exit_code != 0 or grade.violations:
        for item in grade.violations:
            reasons.append(f"rule {item['rule']} {item['name']}: {item['entity_kind']} {item['entity_id']}: {item['reason']}")
        if not grade.violations:
            reasons.append(f"checker exit {grade.exit_code} with conforms={grade.conforms}")
    if grade.report["graded"]["claims"] < 1:
        reasons.append("the checker graded no claims: the run left no trail")
    undone = sorted(slug for slug, status in tasks.items() if status != "done")
    if undone:
        reasons.append("tasks not done: " + ", ".join(f"{slug}={tasks[slug]}" for slug in undone))
    return RunGrade("fail" if reasons else "pass", reasons, grade.report, dict(tasks))


def grade_fixture(fixture: Fixture, planar: Path | None = None) -> RunGrade:
    """Grade the sandbox trail with the installed branch `planar`.

    A database or file the grader cannot read (a missing or corrupt sandbox
    database, an unreadable policy file) is an unreadable report, a fail, not
    an exception that would stop the remaining runs.
    """
    binary = planar or fixture.bin_dir / "planar"
    try:
        tasks = task_statuses(fixture.database, fixture.task_ids)
        report = trail.grade_database(
            fixture.database,
            fixture.anchor_id,
            floor=1,
            config_text=fixture.config.read_text(encoding="utf-8"),
            planar=binary,
        )
    except (trail.TrailError, sqlite3.Error, OSError) as exc:
        return judge(None, str(exc), {})
    return judge(report, None, tasks)


def violations_by_rule(report: Mapping[str, Any] | None) -> dict[str, int]:
    counts: dict[str, int] = {}
    for item in (report or {}).get("violations", []):
        key = f"{item['rule']}:{item['name']}"
        counts[key] = counts.get(key, 0) + 1
    return counts


# --- the ledger -------------------------------------------------------------


def record_ledger(run: RunSpec, artifact_dir: Path, grade: str, model: str | None) -> None:
    """Append one row for `run` to `evals/RESULTS.md` through the harness's writer."""
    import harness  # deferred: the harness module is large and only the ledger needs it

    case = {"id": f"{CASE_ID}/{run.name}", "live": {"host_model": model or ""}}
    options = SimpleNamespace(mode=LEDGER_MODE, vendor=run.vendor, surface=run.surface)
    harness.record_ledger_row(case, artifact_dir, options, grade)  # type: ignore[arg-type]


# --- driving ----------------------------------------------------------------


def vendor_binaries(base: Mapping[str, str] | None = None) -> dict[str, Path]:
    """The vendor CLIs on the harness's own PATH, resolved to real files."""
    source = os.environ if base is None else base
    found: dict[str, Path] = {}
    for name in ("claude", "codex"):
        located = shutil.which(name, path=source.get("PATH"))
        if located:
            found[name] = Path(located).resolve()
    return found


def execute_run(
    run: RunSpec,
    results: Path,
    sandbox_parent: Path,
    *,
    bin_dir: Path,
    timeout_minutes: float,
    model: str | None,
    keep: bool,
    ledger: bool,
) -> dict[str, Any]:
    """Build a fresh sandbox, run one orchestrator in it, grade and record it."""
    artifact_dir = results / run.name
    artifact_dir.mkdir(parents=True, exist_ok=True)
    binaries = vendor_binaries()
    summary: dict[str, Any] = {"run": run.name, "vendor": run.vendor, "surface": run.surface, "model": model or "-"}
    root = Path(tempfile.mkdtemp(prefix=f"acceptance-{run.name}-", dir=sandbox_parent))
    try:
        if run.vendor not in binaries:
            grade = RunGrade("blocked", [f"{run.vendor} is not on PATH"], None, {})
        else:
            fixture = build_sandbox(root, bin_dir=bin_dir, vendor=run.vendor, vendor_binaries=binaries)
            summary["fixture"] = fixture.describe()
            problem = auth_problem(run.vendor, fixture.env, fixture.repo, artifact_dir)
            if problem:
                grade = RunGrade("blocked", [problem], None, {})
            else:
                prompt = prompt_for(run, fixture)
                (artifact_dir / "prompt.txt").write_text(prompt + "\n", encoding="utf-8")
                argv = build_command(run, fixture, prompt, model)
                summary["argv"] = [*argv[:-1], "<prompt.txt>"]
                host = run_host(
                    argv, fixture.env, fixture.repo, timeout_minutes * 60,
                    artifact_dir / "transcript.jsonl", artifact_dir / "stderr.log",
                )
                summary.update(exit_code=host.exit_code, seconds=host.seconds, timed_out=host.timed_out)
                grade = grade_fixture(fixture)
                if host.timed_out:
                    grade.reasons.append(f"the host hit the {timeout_minutes:g}-minute limit")
                    grade.grade = "fail"
                try:
                    trail.snapshot_database(fixture.database, artifact_dir / "planar.db")
                except (trail.TrailError, sqlite3.Error, OSError) as exc:
                    grade.reasons.append(f"the database snapshot was not saved: {exc}")
    except AcceptanceError as exc:
        grade = RunGrade("fail", [f"sandbox: {exc}"], None, {})
    except (sqlite3.Error, OSError) as exc:
        # A database or file error fails this run; the caller's loop goes on to the next one.
        grade = RunGrade("fail", [f"database: {exc}"], None, {})
    finally:
        if keep:
            summary["sandbox"] = str(root)
        else:
            shutil.rmtree(root, ignore_errors=True)
    summary.update(grade.as_json())
    summary["violations_by_rule"] = violations_by_rule(grade.report)
    (artifact_dir / "grade.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    if ledger and grade.grade in ("pass", "fail"):
        record_ledger(run, artifact_dir, grade.grade, model)
    return summary


def artifact_hash(artifact_dir: Path) -> str:
    return hashlib.sha256((artifact_dir / "grade.json").read_bytes()).hexdigest()[:12]


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(prog="acceptance.py", description=__doc__.split("\n\n")[0])
    parser.add_argument("--runs", default=",".join(RUNS), help="comma-separated runs: " + ", ".join(RUNS))
    parser.add_argument("--timeout-minutes", type=float, default=DEFAULT_TIMEOUT_MINUTES)
    parser.add_argument("--results", type=Path, help="artifact directory (default build/eval-acceptance/<time>)")
    parser.add_argument("--sandbox-root", type=Path, default=Path(tempfile.gettempdir()), help="where sandboxes are made")
    parser.add_argument("--bin-dir", type=Path, default=DEFAULT_BIN_DIR, help="the built binaries to bundle")
    parser.add_argument("--claude-model")
    parser.add_argument("--codex-model")
    parser.add_argument("--keep", action="store_true", help="keep each sandbox")
    parser.add_argument("--no-ledger", action="store_true", help="do not append to evals/RESULTS.md")
    parser.add_argument("--fixture-only", action="store_true", help="build one sandbox, check it and stop")
    args = parser.parse_args(list(argv))
    unknown = [name for name in args.runs.split(",") if name not in RUNS]
    if unknown:
        parser.error(f"unknown run(s): {', '.join(unknown)}")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    sandbox_parent = args.sandbox_root.resolve()
    sandbox_parent.mkdir(parents=True, exist_ok=True)
    if args.fixture_only:
        root = Path(tempfile.mkdtemp(prefix="acceptance-fixture-", dir=sandbox_parent))
        try:
            fixture = build_sandbox(root, bin_dir=args.bin_dir)
            grade = grade_fixture(fixture)
            print(json.dumps({"fixture": fixture.describe(), "untouched_trail": grade.as_json()}, indent=2))
        except AcceptanceError as exc:
            print(f"acceptance: fixture failed: {exc}", file=sys.stderr)
            return 1
        finally:
            if not args.keep:
                shutil.rmtree(root, ignore_errors=True)
        return 0
    results = (args.results or DEFAULT_RESULTS_PARENT / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")).resolve()
    results.mkdir(parents=True, exist_ok=True)
    outcomes = []
    for name in args.runs.split(","):
        run = RUNS[name]
        model = args.claude_model if run.vendor == "claude" else args.codex_model
        summary = execute_run(
            run, results, sandbox_parent,
            bin_dir=args.bin_dir.resolve(), timeout_minutes=args.timeout_minutes,
            model=model, keep=args.keep, ledger=not args.no_ledger,
        )
        outcomes.append(summary)
        print(
            f"{name}: {summary['status']} exit={summary.get('exit_code')} seconds={summary.get('seconds')} "
            f"tasks={summary.get('tasks')} violations={summary['violations_by_rule']}"
        )
        for reason in summary["reasons"]:
            print(f"  {reason}")
    print(f"artifacts: {results}")
    return 0 if all(item["status"] == "pass" for item in outcomes) else 1


if __name__ == "__main__":
    sys.exit(main())
