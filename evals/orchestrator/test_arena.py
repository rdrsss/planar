#!/usr/bin/env python3
"""Offline tests for the eval arena and isolation assertion (tech spec 619 C1)."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import arena  # noqa: E402


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
            self.assertTrue((staged / "commands").is_symlink())
            self.assertEqual(
                (staged / "commands" / "marker.txt").read_text(encoding="utf-8"),
                "real\n",
            )
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

    def test_missing_required_claude_surface_fails_closed_naming_the_path(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            real_root = self._fake_real_root(root, "claude", {})  # no `commands`
            env = arena.make_arena(root)
            with self.assertRaises(arena.VendorStagingError) as ctx:
                arena.stage_vendor_config(
                    env, "claude", base_env={"CLAUDE_CONFIG_DIR": str(real_root)}
                )
            self.assertIn(str(real_root / "commands"), str(ctx.exception))

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
            self.assertTrue((staged / "skills").is_symlink())
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

    def test_surface_agent_promotes_agents_to_required_for_claude(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            # `commands` present (otherwise required is skip-defeated), but
            # `agents` absent: with surface="agent" this must fail closed on
            # `agents`, not pass the way surface="skill" does.
            real_root = self._fake_real_root(root, "claude", {"commands": "DIR"})
            env = arena.make_arena(root)
            with self.assertRaises(arena.VendorStagingError) as ctx:
                arena.stage_vendor_config(
                    env,
                    "claude",
                    surface="agent",
                    base_env={"CLAUDE_CONFIG_DIR": str(real_root)},
                )
            self.assertIn(str(real_root / "agents"), str(ctx.exception))

    def test_surface_agent_promotes_agents_to_required_for_codex(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            real_root = self._fake_real_root(root, "codex", {"auth.json": '{"token": "t"}'})
            env = arena.make_arena(root)
            with self.assertRaises(arena.VendorStagingError) as ctx:
                arena.stage_vendor_config(
                    env,
                    "codex",
                    surface="agent",
                    base_env={"CODEX_HOME": str(real_root)},
                )
            self.assertIn(str(real_root / "agents"), str(ctx.exception))

    def test_surface_skill_still_treats_agents_as_optional(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            real_root = self._fake_real_root(root, "claude", {"commands": "DIR"})
            env = arena.make_arena(root)
            # Default surface ("skill"): must not raise despite no `agents`.
            arena.stage_vendor_config(
                env, "claude", base_env={"CLAUDE_CONFIG_DIR": str(real_root)}
            )
            self.assertFalse((Path(env["CLAUDE_CONFIG_DIR"]) / "agents").exists())

    def test_surface_agent_stages_agents_when_present(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            real_root = self._fake_real_root(
                root, "claude", {"commands": "DIR", "agents": "DIR"}
            )
            env = arena.make_arena(root)
            arena.stage_vendor_config(
                env,
                "claude",
                surface="agent",
                base_env={"CLAUDE_CONFIG_DIR": str(real_root)},
            )
            staged = Path(env["CLAUDE_CONFIG_DIR"])
            self.assertTrue((staged / "agents").is_symlink())

    def test_unknown_surface_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="planar-eval-arena-test-") as tmp:
            root = Path(tmp)
            real_root = self._fake_real_root(root, "claude", {"commands": "DIR"})
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
            real_root = self._fake_real_root(root, "claude", {"commands": "DIR"})
            env = arena.make_arena(root)
            arena.stage_vendor_config(
                env, "claude", base_env={"CLAUDE_CONFIG_DIR": str(real_root)}
            )
            arena.assert_isolated(env, root)  # must not raise


if __name__ == "__main__":
    unittest.main()
