# Scriptorium renderer

This C++26 tool stages Planar's unified skill and agent sources for Claude,
Codex, Copilot, and Gemini. `install.sh` builds and installs it at
`$PLANAR_HOME/bin/scriptorium`, calls `render`, and then performs the
vendor-specific links and copies recorded in `install-manifest.json`.

From the repository root:

```sh
cmake --build build/debug --target scriptorium
build/debug/bin/scriptorium render --config scriptorium.yaml --output-root /tmp/planar-stage
build/debug/bin/scriptorium check --config scriptorium.yaml --output-root /tmp/planar-stage
build/debug/bin/scriptorium status --config scriptorium.yaml --output-root /tmp/planar-stage --json
ctest --test-dir build/debug -R scriptorium_projection --output-on-failure
```

Sources under `skills/src/` and `agents/` have YAML frontmatter and an
Inja body. Vendor-specific text uses expressions such as
`{{ VendorTitle }}`. Markdown headings retain their literal `##` prefix.
The tool parses YAML, validates generated TOML, and writes JSON reports through
Glaze. Inja and its
nlohmann dependency are private to this target.

The four skill targets use different layouts. Claude renders flat
`commands/claude/pl-*.md` files that the installer symlinks into
`~/.claude/commands/`. Codex renders `skills/codex/pl-*/SKILL.md` directories;
the installer copies them into `$CODEX_HOME/skills/`. Copilot and Gemini render
flat `skills/<vendor>/pl-*.md` files, which the installer materializes as
`SKILL.md` directories under `~/.copilot/skills/` and
`~/.gemini/antigravity-cli/skills/`, respectively. These paths are relative to
the Scriptorium output root, normally `$PLANAR_HOME`. The repository
[README](../../../README.md#vendor-surfaces) lists the staging directories and
the separate agent surfaces.

`planar-golden.sha256` pins the relative paths and bytes of the 232 current
projections. It was captured from the external Scriptorium before migration,
then updated for the intentional `pl-health` command change requiring an
explicit staging root. The adjacent process test covers parity, drift,
repair through render, and rejection before writes.
