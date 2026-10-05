#!/usr/bin/env python3
"""Offline tests for the eval arena and isolation assertion (tech spec 619 C1)."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import arena  # noqa: E402

_RENDERER = Path(__file__).resolve().parents[2] / "scripts" / "render-codex-agents.py"


class MakeArenaTests(unittest.TestCase):
    def test_every_arena_path_resolves_under_the_root(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            env = arena.make_arena(root)
            resolved_root = root.resolve()
            for key in arena.ARENA_ENV_VARS:
                self.assertIn(key, env, f"{key} missing from arena env")
                resolved = Path(env[key]).resolve()
                self.assertTrue(
                    resolved.is_relative_to(resolved_root),
                    f"{key}={env[key]} does not resolve under arena root {resolved_root}",
                )
            # assert_isolated is the authoritative check; make_arena's own
            # output must satisfy it immediately, with no host started.
            arena.assert_isolated(env, root)

    def test_env_carries_only_the_documented_passthrough_allowlist(self) -> None:
        base_env = {
            "PATH": "/usr/bin:/bin",
            "TERM": "xterm-256color",
            "TMPDIR": "/tmp",
            "LANG": "en_US.UTF-8",
            "LC_ALL": "en_US.UTF-8",
            "ANTHROPIC_API_KEY": "sk-test",
            "OPENAI_API_KEY": "oa-test",
            "CLAUDE_CODE_SOME_FLAG": "1",
            # Not in the allowlist: must not survive into the arena env.
            "SECRET_UNRELATED_TOKEN": "leak-me-not",
            "HOME": "/Users/operator",
        }
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            env = arena.make_arena(root, base_env=base_env)
            self.assertNotIn("SECRET_UNRELATED_TOKEN", env)
            # HOME is arena-owned: the source value must be overridden, never
            # passed through.
            self.assertNotEqual(env["HOME"], base_env["HOME"])
            for key in ("PATH", "TERM", "TMPDIR", "LANG", "LC_ALL", "ANTHROPIC_API_KEY", "OPENAI_API_KEY", "CLAUDE_CODE_SOME_FLAG"):
                self.assertEqual(env[key], base_env[key])
            # Nothing beyond the allowlist union with the arena-owned keys.
            allowed = set(arena.ARENA_ENV_VARS) | {
                "PATH",
                "TERM",
                "TMPDIR",
                "LANG",
                "LC_ALL",
                "ANTHROPIC_API_KEY",
                "OPENAI_API_KEY",
                "CLAUDE_CODE_SOME_FLAG",
            }
            self.assertEqual(set(env), allowed)


class AssertIsolatedTests(unittest.TestCase):
    def _valid_env(self, root: Path) -> dict[str, str]:
        return arena.make_arena(root)

    def test_a_leaked_home_path_fails_before_the_host_starts(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            env = self._valid_env(root)
            env["HOME"] = str(Path.home())
            with self.assertRaises(arena.ArenaIsolationError) as ctx:
                arena.assert_isolated(env, root)
            self.assertIn("HOME", str(ctx.exception))

    def test_each_arena_variable_individually_is_refused_when_leaked(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            outside = Path(tempfile.mkdtemp(prefix="planar-eval-arena-outside-"))
            for key in arena.ARENA_ENV_VARS:
                env = self._valid_env(root)
                env[key] = str(outside / "leaked")
                with self.assertRaises(arena.ArenaIsolationError) as ctx:
                    arena.assert_isolated(env, root)
                self.assertIn(key, str(ctx.exception))

    def test_each_unset_arena_variable_is_refused(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            for key in arena.ARENA_ENV_VARS:
                env = self._valid_env(root)
                del env[key]
                with self.assertRaises(arena.ArenaIsolationError) as ctx:
                    arena.assert_isolated(env, root)
                self.assertIn(key, str(ctx.exception))

    def test_a_valid_arena_env_passes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            env = self._valid_env(root)
            arena.assert_isolated(env, root)  # must not raise

    def test_a_relative_path_value_is_refused(self) -> None:
        # A relative value resolves against the CURRENT process's cwd, not
        # the arena root, so it must be rejected outright rather than
        # resolved and re-checked (which could accidentally land under the
        # root and mask the ambiguity).
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            for key in arena.ARENA_ENV_VARS:
                env = self._valid_env(root)
                env[key] = "relative/child/path"
                with self.assertRaises(arena.ArenaIsolationError) as ctx:
                    arena.assert_isolated(env, root)
                self.assertIn(key, str(ctx.exception))
                self.assertIn("not an absolute path", str(ctx.exception))


class StageVendorConfigTests(unittest.TestCase):
    """Planar question 983: stage read surfaces, never session/state.

    Uses a FAKE "real" vendor dir under a TemporaryDirectory throughout —
    never the operator's actual ~/.claude or ~/.codex.
    """

    @staticmethod
    def _fake_real_root(tmp: Path, vendor: str, present: dict[str, str]) -> Path:
        real_root = tmp / f"real-{vendor}"
        real_root.mkdir()
        for name, value in present.items():
            target = real_root / name
            if value == "DIR":
                target.mkdir(parents=True, exist_ok=True)
                (target / "marker.txt").write_text("real\n", encoding="utf-8")
            else:
                target.write_text(value, encoding="utf-8")
        return real_root

    def test_claude_symlinks_resolve_to_the_real_install(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            real_root = self._fake_real_root(
                root,
                "claude",
                {
                    # Present on the real install, but the Planar surface
                    # comes from the checkout, never from these.
                    "commands": "DIR",
                    "skills": "DIR",
                    "settings.json": '{"x": 1}',
                    "CLAUDE.md": "# notes\n",
                    # Present on the real install, but must NEVER be
                    # staged — see the write-leak test below.
                    "plugins": "DIR",
                },
            )
            env = arena.make_arena(root)
            arena.stage_vendor_config(
                env, "claude", base_env={"CLAUDE_CONFIG_DIR": str(real_root)}
            )
            staged = Path(env["CLAUDE_CONFIG_DIR"])
            self.assertFalse((staged / "commands").exists())
            self.assertFalse((staged / "skills").exists())
            self.assertTrue((staged / "settings.json").is_symlink())
            self.assertEqual(
                (staged / "settings.json").read_text(encoding="utf-8"), '{"x": 1}'
            )
            # Not present on the fake real install: skipped, not fabricated.
            self.assertFalse((staged / "agents").exists())
            self.assertFalse((staged / ".credentials.json").exists())
            # Present on the fake real install, but must never be staged:
            # Claude rewrites plugins/.last_inuse_sweep and
            # known_marketplaces.json on ordinary session start, which
            # would leak a host write into the real install through a
            # symlink.
            self.assertFalse((staged / "plugins").exists())

    def test_missing_planar_skill_in_the_checkout_fails_closed_naming_the_path(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            real_root = self._fake_real_root(root, "claude", {})
            repo = root / "repo"
            repo.mkdir()
            env = arena.make_arena(root)
            with self.assertRaises(arena.VendorStagingError) as ctx:
                arena.stage_vendor_config(
                    env,
                    "claude",
                    base_env={"CLAUDE_CONFIG_DIR": str(real_root)},
                    repo_root=repo,
                )
            self.assertIn(str(repo / "skills" / "planar" / "SKILL.md"), str(ctx.exception))

    def test_missing_required_codex_auth_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            real_root = self._fake_real_root(root, "codex", {"config.toml": "x=1\n"})
            env = arena.make_arena(root)
            with self.assertRaises(arena.VendorStagingError) as ctx:
                arena.stage_vendor_config(
                    env, "codex", base_env={"CODEX_HOME": str(real_root)}
                )
            self.assertIn("auth.json", str(ctx.exception))

    def test_codex_stages_auth_and_skips_missing_optionals(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            real_root = self._fake_real_root(
                root,
                "codex",
                {
                    "auth.json": '{"token": "t"}',
                    "skills": "DIR",
                    # Present on the real install, but must never be
                    # staged (same write-leak concern as claude's).
                    "plugins": "DIR",
                },
            )
            env = arena.make_arena(root)
            arena.stage_vendor_config(
                env, "codex", base_env={"CODEX_HOME": str(real_root)}
            )
            staged = Path(env["CODEX_HOME"])
            self.assertTrue((staged / "auth.json").is_symlink())
            self.assertEqual(
                (staged / "auth.json").read_text(encoding="utf-8"), '{"token": "t"}'
            )
            self.assertFalse((staged / "skills").exists())
            self.assertFalse((staged / "config.toml").exists())
            self.assertFalse((staged / "AGENTS.md").exists())
            self.assertFalse((staged / "plugins").exists())

    def test_nothing_under_the_excluded_state_list_is_ever_staged(self) -> None:
        for vendor, surfaces in (
            ("claude", arena.CLAUDE_STAGED_SURFACES),
            ("codex", arena.CODEX_STAGED_SURFACES),
        ):
            for excluded in arena.EXCLUDED_STATE_SURFACES:
                self.assertNotIn(
                    excluded, surfaces, f"{vendor} must never stage {excluded}"
                )

    def test_plugins_is_never_a_staged_surface(self) -> None:
        # Both vendors rewrite state under plugins/ on ordinary session
        # start (e.g. claude's .last_inuse_sweep and
        # known_marketplaces.json), so a symlinked plugins/ would leak a
        # host write into the operator's real install — the one thing
        # q983's "every write lands in scratch" promise exists to prevent.
        # It is excluded outright, not merely marked optional.
        self.assertNotIn("plugins", arena.CLAUDE_STAGED_SURFACES)
        self.assertNotIn("plugins", arena.CODEX_STAGED_SURFACES)

    def test_defaults_to_the_real_home_when_no_env_override_is_set(self) -> None:
        # No CLAUDE_CONFIG_DIR / CODEX_HOME in base_env: falls back to the
        # vendor's documented default under the operator's real home, never
        # a scratch or arbitrary path.
        self.assertEqual(
            arena.real_vendor_root("claude", base_env={}), Path.home() / ".claude"
        )
        self.assertEqual(
            arena.real_vendor_root("codex", base_env={}), Path.home() / ".codex"
        )

    def test_unknown_vendor_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            arena.real_vendor_root("bogus-vendor")
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            env = arena.make_arena(root)
            with self.assertRaises(ValueError):
                arena.stage_vendor_config(env, "bogus-vendor")

    @staticmethod
    def _fake_repo(tmp: Path, agents: tuple[str, ...] = ("planar-orchestrator",)) -> Path:
        repo = tmp / "repo"
        (repo / "skills" / "planar" / "references").mkdir(parents=True)
        (repo / "skills" / "planar" / "SKILL.md").write_text("# planar\n", encoding="utf-8")
        (repo / "skills" / "planar" / "references" / "x.md").write_text("x\n", encoding="utf-8")
        (repo / "agents").mkdir()
        for name in agents:
            (repo / "agents" / f"{name}.md").write_text(
                f"---\nname: {name}\ndescription: Does {name} work.\n"
                f"planar:\n  kind: agent\n  slug: {name}\n---\n\nBody of {name}.\n",
                encoding="utf-8",
            )
        (repo / "scripts").mkdir()
        shutil.copyfile(_RENDERER, repo / "scripts" / "render-codex-agents.py")
        # Legacy unprefixed agent must never be staged.
        (repo / "agents" / "coder.md").write_text(
            "---\nname: coder\ndescription: legacy\n---\n\nlegacy\n", encoding="utf-8"
        )
        (repo / "commands").mkdir()
        (repo / "commands" / "pl-x.md").write_text("legacy\n", encoding="utf-8")
        return repo

    def test_claude_checkout_surface_is_staged_under_the_arena_home(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            real_root = self._fake_real_root(root, "claude", {})
            repo = self._fake_repo(root)
            env = arena.make_arena(root)
            arena.stage_vendor_config(
                env,
                "claude",
                surface="agent",
                base_env={"CLAUDE_CONFIG_DIR": str(real_root)},
                repo_root=repo,
                required_agents=("planar-orchestrator",),
            )
            home = Path(env["HOME"])
            self.assertEqual(
                (home / ".claude" / "skills" / "planar" / "SKILL.md").read_text(encoding="utf-8"),
                "# planar\n",
            )
            self.assertEqual(
                (home / ".claude" / "skills" / "planar" / "references" / "x.md").read_text(
                    encoding="utf-8"
                ),
                "x\n",
            )
            self.assertTrue((home / ".claude" / "agents" / "planar-orchestrator.md").is_file())
            self.assertTrue((home / ".agents" / "skills" / "planar" / "SKILL.md").is_file())
            # The same files at the real CLAUDE_CONFIG_DIR the host reads.
            self.assertTrue(
                (Path(env["CLAUDE_CONFIG_DIR"]) / "skills" / "planar" / "SKILL.md").is_file()
            )
            self.assertFalse((home / ".claude" / "commands").exists())
            self.assertFalse((home / ".claude" / "agents" / "coder.md").exists())
            self.assertFalse((home / ".agents" / "commands").exists())

    def test_codex_checkout_agents_are_generated_as_toml(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            real_root = self._fake_real_root(root, "codex", {"auth.json": "{}"})
            repo = self._fake_repo(root)
            env = arena.make_arena(root)
            arena.stage_vendor_config(
                env,
                "codex",
                surface="agent",
                base_env={"CODEX_HOME": str(real_root)},
                repo_root=repo,
                required_agents=("planar-orchestrator",),
            )
            home = Path(env["HOME"])
            toml_path = home / ".codex" / "agents" / "planar-orchestrator.toml"
            toml_text = toml_path.read_text(encoding="utf-8")
            self.assertIn('name = "planar-orchestrator"\n', toml_text)
            self.assertIn('description = "Does planar-orchestrator work."\n', toml_text)
            self.assertIn("developer_instructions = ", toml_text)
            self.assertIn("Body of planar-orchestrator.", toml_text)
            self.assertNotIn("description: ", toml_text)
            self.assertTrue((home / ".agents" / "skills" / "planar" / "SKILL.md").is_file())
            self.assertFalse((home / ".codex" / "agents" / "coder.toml").exists())
            self.assertFalse((home / ".codex" / "commands").exists())

    def test_codex_toml_bytes_equal_the_renderer_script_output(self) -> None:
        # One TOML emitter: the arena stages exactly what the script writes.
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            real_root = self._fake_real_root(root, "codex", {"auth.json": "{}"})
            repo = self._fake_repo(root, ("planar-orchestrator", "planar-coder"))
            env = arena.make_arena(root)
            arena.stage_vendor_config(
                env,
                "codex",
                surface="agent",
                base_env={"CODEX_HOME": str(real_root)},
                repo_root=repo,
                required_agents=("planar-orchestrator",),
            )
            out = root / "script-out"
            subprocess.run(
                [sys.executable, str(_RENDERER), str(repo / "agents"), str(out)],
                check=True,
                capture_output=True,
            )
            expected = sorted(out.glob("planar-*.toml"))
            self.assertEqual([p.name for p in expected], ["planar-coder.toml", "planar-orchestrator.toml"])
            staged_dir = Path(env["HOME"]) / ".codex" / "agents"
            self.assertEqual(
                sorted(p.name for p in staged_dir.glob("*.toml")), [p.name for p in expected]
            )
            for rendered in expected:
                self.assertEqual((staged_dir / rendered.name).read_bytes(), rendered.read_bytes())

    def test_missing_codex_renderer_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            real_root = self._fake_real_root(root, "codex", {"auth.json": "{}"})
            repo = self._fake_repo(root)
            (repo / "scripts" / "render-codex-agents.py").unlink()
            env = arena.make_arena(root)
            with self.assertRaises(arena.VendorStagingError) as ctx:
                arena.stage_vendor_config(
                    env,
                    "codex",
                    surface="agent",
                    base_env={"CODEX_HOME": str(real_root)},
                    repo_root=repo,
                    required_agents=("planar-orchestrator",),
                )
            self.assertIn("render-codex-agents.py", str(ctx.exception))

    def test_missing_required_agent_in_the_checkout_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            real_root = self._fake_real_root(root, "claude", {})
            repo = self._fake_repo(root, agents=())
            env = arena.make_arena(root)
            with self.assertRaises(arena.VendorStagingError) as ctx:
                arena.stage_vendor_config(
                    env,
                    "claude",
                    surface="agent",
                    base_env={"CLAUDE_CONFIG_DIR": str(real_root)},
                    repo_root=repo,
                    required_agents=("planar-orchestrator",),
                )
            self.assertIn(
                str(repo / "agents" / "planar-orchestrator.md"), str(ctx.exception)
            )

    def test_without_repo_root_no_planar_surface_is_staged(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            real_root = self._fake_real_root(root, "claude", {})
            env = arena.make_arena(root)
            arena.stage_vendor_config(
                env, "claude", base_env={"CLAUDE_CONFIG_DIR": str(real_root)}
            )
            self.assertFalse((Path(env["HOME"]) / ".agents").exists())
            self.assertFalse((Path(env["CLAUDE_CONFIG_DIR"]) / "agents").exists())

    def test_unknown_surface_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            real_root = self._fake_real_root(root, "claude", {})
            env = arena.make_arena(root)
            with self.assertRaises(ValueError):
                arena.stage_vendor_config(
                    env,
                    "claude",
                    surface="bogus",
                    base_env={"CLAUDE_CONFIG_DIR": str(real_root)},
                )

    def test_assert_isolated_ignores_symlink_targets_outside_the_root(self) -> None:
        # The staged symlinks point OUTSIDE the arena root by design.
        # assert_isolated must still pass: it validates the arena env var
        # VALUES (the scratch directories themselves), not what a symlink
        # inside one of them resolves to.
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            real_root = self._fake_real_root(root, "claude", {})
            env = arena.make_arena(root)
            arena.stage_vendor_config(
                env, "claude", base_env={"CLAUDE_CONFIG_DIR": str(real_root)}
            )
            arena.assert_isolated(env, root)  # must not raise


class AssertVendorAuthTests(unittest.TestCase):
    """Planar artifact 626 / task 6872: staging is not authenticating.

    Keychain-backed `claude` login does not follow into a scratch
    `CLAUDE_CONFIG_DIR`, so `assert_vendor_auth()` must fail closed unless
    the arena env (or, for codex, the staged config dir) carries an
    explicit auth surface.
    """

    def test_claude_passes_with_oauth_token(self) -> None:
        arena.assert_vendor_auth(
            {"CLAUDE_CODE_OAUTH_TOKEN": "sk-ant-oat-test-token"}, "claude"
        )  # must not raise

    def test_claude_passes_with_api_key(self) -> None:
        arena.assert_vendor_auth({"ANTHROPIC_API_KEY": "sk-ant-test-key"}, "claude")

    def test_claude_fails_closed_with_neither_surface(self) -> None:
        with self.assertRaises(arena.VendorAuthError) as ctx:
            arena.assert_vendor_auth({}, "claude")
        message = str(ctx.exception)
        self.assertIn("CLAUDE_CODE_OAUTH_TOKEN", message)
        self.assertIn("ANTHROPIC_API_KEY", message)
        self.assertIn("claude setup-token", message)

    def test_claude_fails_closed_on_empty_string_values(self) -> None:
        # An empty-string value is "unset" for auth purposes, not present.
        with self.assertRaises(arena.VendorAuthError):
            arena.assert_vendor_auth(
                {"CLAUDE_CODE_OAUTH_TOKEN": "", "ANTHROPIC_API_KEY": ""}, "claude"
            )

    def test_claude_error_never_contains_the_token_value(self) -> None:
        # Seed a fake token that would be an obvious leak if echoed back.
        fake_token = "sk-ant-oat-DO-NOT-LEAK-THIS-VALUE-0000000000"
        with self.assertRaises(arena.VendorAuthError) as ctx:
            # The token is absent from env entirely here (the failure
            # case); this proves the message construction path never
            # embeds a token value even incidentally.
            arena.assert_vendor_auth({"SOME_OTHER_VAR": fake_token}, "claude")
        self.assertNotIn(fake_token, str(ctx.exception))

    def test_codex_passes_with_api_key(self) -> None:
        arena.assert_vendor_auth({"OPENAI_API_KEY": "sk-oa-test-key"}, "codex")

    def test_codex_passes_with_staged_auth_json(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            codex_home = Path(tmp) / "codex-home"
            codex_home.mkdir()
            (codex_home / "auth.json").write_text('{"token": "t"}', encoding="utf-8")
            arena.assert_vendor_auth({"CODEX_HOME": str(codex_home)}, "codex")

    def test_codex_fails_closed_with_neither_surface(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            codex_home = Path(tmp) / "codex-home"
            codex_home.mkdir()  # no auth.json staged
            with self.assertRaises(arena.VendorAuthError) as ctx:
                arena.assert_vendor_auth({"CODEX_HOME": str(codex_home)}, "codex")
            message = str(ctx.exception)
            self.assertIn("auth.json", message)
            self.assertIn("OPENAI_API_KEY", message)

    def test_codex_fails_closed_with_no_codex_home_at_all(self) -> None:
        with self.assertRaises(arena.VendorAuthError):
            arena.assert_vendor_auth({}, "codex")

    def test_codex_error_never_contains_the_api_key_value(self) -> None:
        fake_key = "sk-oa-DO-NOT-LEAK-THIS-VALUE-1111111111"
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            codex_home = Path(tmp) / "codex-home"
            codex_home.mkdir()
            with self.assertRaises(arena.VendorAuthError) as ctx:
                arena.assert_vendor_auth(
                    {"CODEX_HOME": str(codex_home), "SOME_OTHER_VAR": fake_key}, "codex"
                )
            self.assertNotIn(fake_key, str(ctx.exception))

    def test_unknown_vendor_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            arena.assert_vendor_auth({}, "bogus-vendor")


if __name__ == "__main__":
    unittest.main()
