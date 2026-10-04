#!/usr/bin/env bash
# Installer staging-step fixtures (plan 1104, M2): install.sh stages
# skills/planar/ and agents/*.md under $PLANAR_HOME and renders the Codex agent
# TOML into codex-agents/, before any vendor placement. Every run is against a
# scratch copy of the installer, a scratch HOME and prefix, and a stub `cmake`
# and stub binaries, so nothing is built and the operator's ~/.planar is never
# looked at.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { printf 'install-stage-test: %s\n' "$*" >&2; exit 1; }

# A scratch checkout: the installer, its scripts, the real skill and agents,
# and the minimum else the installer checks for. The pinned compiler paths are
# pointed at a file that exists everywhere, as install-deps-test.sh points them
# at a missing one.
make_repo() {
  local repo="$1"
  mkdir -p "$repo/skills/src" "$repo/skills"
  cp "$ROOT/install.sh" "$repo/install.sh"
  cp -R "$ROOT/scripts" "$repo/scripts"
  cp -R "$ROOT/agents" "$repo/agents"
  cp -R "$ROOT/skills/planar" "$repo/skills/planar"
  : > "$repo/CMakeLists.txt"
  : > "$repo/CMakePresets.json"
  : > "$repo/scriptorium.yaml"
  printf 'x\n' > "$repo/skills/src/pl-x.md"
  perl -0pi -e 's#/opt/homebrew/opt/llvm/bin/clang(\+\+)?#/bin/sh#g' "$repo/install.sh"
}

# Stub cmake and binaries. `cmake --install D --prefix P` puts the stubs in
# P/bin. The stub scriptorium stands in for the retired render: it writes
# agents/codex/legacy.toml, so the test can tell what the staging step added to
# agents/codex/ (nothing) from what the render did.
STUBS="$TMP/stubs"
mkdir -p "$STUBS"
cat > "$STUBS/cmake" <<'STUB'
#!/usr/bin/env bash
if [[ "$1" == "--install" ]]; then
  prefix="$4"
  mkdir -p "$prefix/bin"
  for b in planar planar-agent planar-watch planar-execute planar-ext; do
    printf '#!/bin/sh\n[ "$1" = version ] && echo "planar stage-test"\nexit 0\n' > "$prefix/bin/$b"
    chmod +x "$prefix/bin/$b"
  done
  cat > "$prefix/bin/scriptorium" <<'SCRIPTORIUM'
#!/bin/sh
case "$1" in
  version) echo "scriptorium stage-test" ;;
  render)
    mkdir -p agents/claude agents/codex commands/claude
    echo legacy > agents/codex/legacy.toml
    echo cmd > commands/claude/pl-x.md
    echo agent > agents/claude/x.md
    ;;
esac
exit 0
SCRIPTORIUM
  chmod +x "$prefix/bin/scriptorium"
fi
exit 0
STUB
chmod +x "$STUBS/cmake"

# run_install REPO HOME [FLAGS...] -- stdout/stderr land in $HOME/{out,err}.
run_install() {
  local repo="$1" home="$2"; shift 2
  mkdir -p "$home"
  PATH="$STUBS:$PATH" HOME="$home" PLANAR_HOME="$home/.planar" CODEX_HOME="$home/.codex" \
    PLANAR_DB="$home/planar.db" NO_COLOR=1 \
    "$repo/install.sh" --build-dir "$home/build" "$@" >"$home/out" 2>"$home/err"
}

ROLES=(coder ext-sync feedback-triager importer ingestor introspector janitor orchestrator planner research reviewer spec-reviewer sync-reconciler synthesizer test-coder)
DOCS=(cross-scope-writes doctrine methodology models)

check_staged() {
  local repo="$1" home="$2" prefix="$2/.planar" r d f
  [[ -f "$prefix/skills/planar/SKILL.md" ]] || fail "SKILL.md was not staged"
  cmp -s "$repo/skills/planar/SKILL.md" "$prefix/skills/planar/SKILL.md" || fail "staged SKILL.md differs"
  for f in "$repo"/skills/planar/references/*.md; do
    cmp -s "$f" "$prefix/skills/planar/references/$(basename "$f")" || fail "staged $(basename "$f") differs or is missing"
  done
  [[ "$(find "$prefix/skills/planar/" -type f | grep -c .)" == "$(find "$repo/skills/planar/" -type f | grep -c .)" ]] \
    || fail "the staged skill has a different file count than the checkout"
  [[ "${#ROLES[@]}" == 15 && "${#DOCS[@]}" == 4 ]] || fail "role/doc fixtures are not 15 + 4"
  for r in "${ROLES[@]}"; do
    cmp -s "$repo/agents/planar-$r.md" "$prefix/agents/planar-$r.md" || fail "agents/planar-$r.md was not staged"
    [[ -f "$prefix/codex-agents/planar-$r.toml" ]] || fail "codex-agents/planar-$r.toml was not rendered"
    python3 - "$prefix/codex-agents/planar-$r.toml" "planar-$r" <<'PY' || fail "codex-agents/planar-$r.toml does not parse"
import sys
try:
    import tomllib
except ImportError:  # Python < 3.11: the renderer writes exactly three one-line keys
    lines = open(sys.argv[1], encoding='utf-8').read().split('\n')
    assert lines.pop() == '' and len(lines) == 3, lines
    for line, key in zip(lines, ('name', 'description', 'developer_instructions')):
        assert line.startswith(key + ' = "') and line.endswith('"'), line
    assert lines[0] == 'name = "%s"' % sys.argv[2]
else:
    d = tomllib.load(open(sys.argv[1], 'rb'))
    assert list(d) == ['name', 'description', 'developer_instructions'], list(d)
    assert d['name'] == sys.argv[2]
PY
  done
  for d in "${DOCS[@]}"; do
    cmp -s "$repo/agents/$d.md" "$prefix/agents/$d.md" || fail "agents/$d.md was not staged"
  done
  [[ "$(find "$prefix/codex-agents" -type f | grep -c .)" == 15 ]] || fail "codex-agents/ does not hold exactly the fifteen TOML files"
  [[ ! -e "$prefix/codex-agents.new" ]] || fail "the staging temp directory was left behind"
  # The staging step added nothing to agents/codex/: the only file there is
  # the one the (stub) render writes.
  [[ "$(ls -A "$prefix/agents/codex" | tr '\n' ' ')" == "legacy.toml " ]] \
    || fail "agents/codex/ holds more than the render's file: $(ls -A "$prefix/agents/codex" | tr '\n' ' ')"
  # The manifest records each staged path.
  local m="$prefix/install-manifest.json"
  grep -Fq '"skills/planar/SKILL.md"' "$m" || fail "manifest lacks skills/planar/SKILL.md"
  for f in "$repo"/skills/planar/references/*.md; do
    grep -Fq "\"skills/planar/references/$(basename "$f")\"" "$m" || fail "manifest lacks $(basename "$f")"
  done
  for r in "${ROLES[@]}"; do
    grep -Fq "\"agents/planar-$r.md\"" "$m" || fail "manifest lacks agents/planar-$r.md"
    grep -Fq "\"codex-agents/planar-$r.toml\"" "$m" || fail "manifest lacks codex-agents/planar-$r.toml"
  done
  for d in "${DOCS[@]}"; do grep -Fq "\"agents/$d.md\"" "$m" || fail "manifest lacks agents/$d.md"; done
  grep -Fq '"agents/codex/' "$m" && fail "manifest records something under agents/codex/"
  return 0
}

# 1. Copy mode, no vendor: everything is staged, and the (stub) render still
# runs after it.
REPO="$TMP/repo"; make_repo "$REPO"
run_install "$REPO" "$TMP/h1" --no-vendor || fail "install failed: $(cat "$TMP/h1/err")"
check_staged "$REPO" "$TMP/h1"
[[ -f "$TMP/h1/.planar/commands/claude/pl-x.md" ]] || fail "the scriptorium render no longer runs"
[[ ! -L "$TMP/h1/.planar/skills/planar" && ! -L "$TMP/h1/.planar/agents/planar-coder.md" ]] || fail "copy mode staged symlinks"
stage_line="$(grep -n 'Staging the planar skill and agents' "$TMP/h1/out" | head -1 | cut -d: -f1 || true)"
render_line="$(grep -n 'Rendering per-vendor skill outputs' "$TMP/h1/out" | head -1 | cut -d: -f1 || true)"
[[ -n "$stage_line" && -n "$render_line" && "$stage_line" -lt "$render_line" ]] || fail "staging does not come before the render"

# 2. A re-install replaces stale staged files and leaves agents/<vendor>/ alone.
echo stale > "$TMP/h1/.planar/skills/planar/references/stale.md"
echo stale > "$TMP/h1/.planar/codex-agents/stale.toml"
run_install "$REPO" "$TMP/h1" --no-vendor || fail "re-install failed: $(cat "$TMP/h1/err")"
[[ ! -e "$TMP/h1/.planar/skills/planar/references/stale.md" ]] || fail "a re-install kept a stale staged reference"
[[ ! -e "$TMP/h1/.planar/codex-agents/stale.toml" ]] || fail "a re-install kept a stale TOML"
check_staged "$REPO" "$TMP/h1"

# 3. With a vendor selected the installer stays runnable end to end, and the
# manifest keeps its projection rows next to the staged paths.
run_install "$REPO" "$TMP/h3" --vendors claude || fail "install with --vendors claude failed: $(cat "$TMP/h3/err")"
check_staged "$REPO" "$TMP/h3"
[[ -L "$TMP/h3/.claude/commands/pl-x.md" ]] || fail "the claude vendor block no longer places the render's output"
grep -Fq '"vendor": "claude"' "$TMP/h3/.planar/install-manifest.json" || fail "the vendor projection rows are gone from the manifest"
! grep -Fq '"vendor": "planar"' "$TMP/h3/.planar/install-manifest.json" || fail "a staged path was recorded as a projection row"

# 4. Link mode: the staged trees point into the checkout.
run_install "$REPO" "$TMP/h4" --no-vendor --link || fail "link install failed: $(cat "$TMP/h4/err")"
[[ -L "$TMP/h4/.planar/skills/planar" ]] || fail "link mode did not link skills/planar"
[[ -L "$TMP/h4/.planar/agents/planar-coder.md" ]] || fail "link mode did not link the agents"
check_staged "$REPO" "$TMP/h4"

# 5. A malformed agent stops the installer at the staging step, naming the
# file, before the render, and leaves no codex-agents/.
BAD="$TMP/badrepo"; make_repo "$BAD"
printf 'no frontmatter at all\n' > "$BAD/agents/planar-broken.md"
if run_install "$BAD" "$TMP/h5" --no-vendor; then fail "a malformed agent did not stop the installer"; fi
grep -Fq 'planar-broken.md' "$TMP/h5/err" || fail "the failure does not name the malformed file: $(cat "$TMP/h5/err")"
grep -Fq 'Staging the planar skill and agents' "$TMP/h5/out" || fail "the failure was not at the staging step"
! grep -Fq 'Rendering per-vendor skill outputs' "$TMP/h5/out" || fail "the installer went past the staging step"
[[ ! -e "$TMP/h5/.planar/codex-agents" && ! -e "$TMP/h5/.planar/codex-agents.new" ]] || fail "a failed render left codex-agents/"
[[ ! -e "$TMP/h5/.planar/install-manifest.json" ]] || fail "a failed staging step still wrote the manifest"

# 6. The manifest function, directly: it records extras only, never projection
# rows, so the installed-surface reader's schema is untouched.
# shellcheck source=install-manifest.sh
source "$ROOT/scripts/install-manifest.sh"
install_manifest_begin "build-staged" copy
install_manifest_record_staged "$TMP/h1/.planar" "$REPO"
install_manifest_write "$TMP/staged-manifest.json"
grep -Fq '"projections": [' "$TMP/staged-manifest.json" || fail "the manifest lost its projections array"
[[ "${#INSTALL_MANIFEST_ROW_VENDOR[@]}" == 0 ]] || fail "staged paths were recorded as projection rows"
[[ "${#INSTALL_MANIFEST_EXTRAS[@]}" == $(( $(find "$REPO/skills/planar" -type f | grep -c .) + 19 + 15 )) ]] \
  || fail "unexpected extras count: ${#INSTALL_MANIFEST_EXTRAS[@]}"

printf 'install-stage tests: 6 passed\n'
