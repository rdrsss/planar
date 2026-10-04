#!/usr/bin/env bash
# Installer staging and vendor-surface fixtures (plan 1104, M2): install.sh
# stages skills/planar/ and agents/*.md under $PLANAR_HOME, renders the Codex
# agent TOML into codex-agents/, then places them into each vendor found on the
# host (six vendors, nine targets, one presence marker each). Every run is
# against a scratch copy of the installer, a scratch HOME and prefix, and a stub
# `cmake` and stub binaries, so nothing is built and the operator's ~/.planar is
# never looked at.
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
# P/bin. The stub scriptorium stands in for the retired render: if the installer
# still ran `scriptorium render` it would write agents/codex/legacy.toml and
# commands/claude/pl-x.md, which the scenarios assert are absent.
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
  # CODEX_HOME is unset unless the scenario sets RUN_CODEX_HOME: Codex's
  # presence marker is "CODEX_HOME set, else ~/.codex/ exists".
  env -u CODEX_HOME ${RUN_CODEX_HOME:+CODEX_HOME="$RUN_CODEX_HOME"} \
    PATH="$STUBS:$PATH" HOME="$home" PLANAR_HOME="$home/.planar" \
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
  # Nothing renders into agents/<vendor>/ any more, and the TOML never lands
  # under agents/codex/.
  [[ ! -e "$prefix/agents/codex" && ! -e "$prefix/agents/claude" ]] || fail "agents/<vendor>/ exists: $(ls "$prefix/agents")"
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

# 1. Copy mode, no vendor: everything is staged, and the scriptorium render no
# longer runs.
REPO="$TMP/repo"; make_repo "$REPO"
run_install "$REPO" "$TMP/h1" --no-vendor || fail "install failed: $(cat "$TMP/h1/err")"
check_staged "$REPO" "$TMP/h1"
[[ ! -e "$TMP/h1/.planar/commands" ]] || fail "the installer still runs the scriptorium render"
[[ ! -L "$TMP/h1/.planar/skills/planar" && ! -L "$TMP/h1/.planar/agents/planar-coder.md" ]] || fail "copy mode staged symlinks"
grep -Fq 'Staging the planar skill and agents' "$TMP/h1/out" || fail "the staging step did not run"
! grep -Fq 'Rendering per-vendor skill outputs' "$TMP/h1/out" || fail "the render section is still in the installer"
[[ ! -e "$TMP/h1/.claude" && ! -e "$TMP/h1/.agents" ]] || fail "--no-vendor placed something in the home directory"

# 2. A re-install replaces stale staged files and leaves agents/<vendor>/ alone.
echo stale > "$TMP/h1/.planar/skills/planar/references/stale.md"
echo stale > "$TMP/h1/.planar/codex-agents/stale.toml"
run_install "$REPO" "$TMP/h1" --no-vendor || fail "re-install failed: $(cat "$TMP/h1/err")"
[[ ! -e "$TMP/h1/.planar/skills/planar/references/stale.md" ]] || fail "a re-install kept a stale staged reference"
[[ ! -e "$TMP/h1/.planar/codex-agents/stale.toml" ]] || fail "a re-install kept a stale TOML"
check_staged "$REPO" "$TMP/h1"

# 3. With a vendor present and selected the installer stays runnable end to
# end; placed paths go to `extras`, never to projection rows.
mkdir -p "$TMP/h3/.claude"
run_install "$REPO" "$TMP/h3" --vendors claude || fail "install with --vendors claude failed: $(cat "$TMP/h3/err")"
check_staged "$REPO" "$TMP/h3"
[[ -f "$TMP/h3/.claude/skills/planar/SKILL.md" && -f "$TMP/h3/.claude/agents/planar-coder.md" ]] || fail "the claude vendor was not placed"
[[ ! -e "$TMP/h3/.claude/commands" ]] || fail "the old ~/.claude/commands target is still written"
! grep -Fq '"vendor":' "$TMP/h3/.planar/install-manifest.json" || fail "a placed path was recorded as a projection row"

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


# ---------------------------------------------------------------------------
# Vendor surfaces: six vendors, nine targets, one presence marker each.
# ---------------------------------------------------------------------------

# mk_home NAME MARKER... -- a scratch home seeded with presence markers.
mk_home() {
  local home="$TMP/$1"; shift
  mkdir -p "$home"
  local m
  for m in "$@"; do
    case "$m" in
      claude)      mkdir -p "$home/.claude" ;;
      codex)       mkdir -p "$home/.codex" ;;
      copilot)     mkdir -p "$home/.copilot" ;;
      gemini)      mkdir -p "$home/.gemini"; : > "$home/.gemini/settings.json" ;;
      antigravity) mkdir -p "$home/.gemini/antigravity-cli" ;;
      opencode)    mkdir -p "$home/.config/opencode" ;;
      *) fail "unknown marker $m" ;;
    esac
  done
}

# placed_set HOME -- every file Planar placed outside the prefix, following
# links so a linked skill directory lists its files like a copied one does.
placed_set() {
  (cd "$1" && find -L . -type f \
    ! -path './.planar/*' ! -path './build/*' ! -path './out' ! -path './err' \
    ! -path './planar.db*' ! -path './.gemini/settings.json' | sort)
}

SKILL_FILES="$(cd "$REPO/skills/planar" && find . -type f | sed 's#^\./##' | sort)"

# expected_set VENDOR... -- the files the given vendors' targets must hold.
expected_set() {
  local v r f shared=0
  for v in "$@"; do
    case "$v" in codex|copilot|gemini|opencode) shared=1 ;; esac
    case "$v" in
      claude)      while read -r f; do echo "./.claude/skills/planar/$f"; done <<< "$SKILL_FILES"
                   for r in "${ROLES[@]}"; do echo "./.claude/agents/planar-$r.md"; done ;;
      codex)       for r in "${ROLES[@]}"; do echo "./.codex/agents/planar-$r.toml"; done ;;
      copilot)     for r in "${ROLES[@]}"; do echo "./.copilot/agents/planar-$r.agent.md"; done ;;
      gemini)      for r in "${ROLES[@]}"; do echo "./.gemini/agents/planar-$r.md"; done ;;
      antigravity) while read -r f; do echo "./.gemini/antigravity-cli/skills/planar/$f"; done <<< "$SKILL_FILES"
                   for r in "${ROLES[@]}"; do echo "./.gemini/antigravity-cli/agents/planar-$r.md"; done ;;
      opencode)    for r in "${ROLES[@]}"; do echo "./.config/opencode/agents/planar-$r.md"; done ;;
    esac
  done
  [[ "$shared" -eq 0 ]] || while read -r f; do echo "./.agents/skills/planar/$f"; done <<< "$SKILL_FILES"
}

# assert_placed HOME VENDOR... -- the placed files are exactly the expected set.
assert_placed() {
  local home="$1"; shift
  diff <(expected_set "$@" | sort) <(placed_set "$home") >"$TMP/diff" \
    || fail "placed files differ for [$*] in $home (< expected, > actual): $(head -20 "$TMP/diff")"
}

assert_found() {  # HOME "vendors as printed"
  grep -Fxq "  vendors found:   $2" "$1/out" || fail "installer did not print 'vendors found: $2': $(grep 'vendors' "$1/out")"
}

# check_formats REPO HOME VENDOR... -- the content of each placed file.
check_formats() {
  local repo="$1" home="$2"; shift 2
  local v r f
  for v in "$@"; do
    for r in "${ROLES[@]}"; do
      case "$v" in
        claude)      f="$home/.claude/agents/planar-$r.md" ;;
        gemini)      f="$home/.gemini/agents/planar-$r.md" ;;
        antigravity) f="$home/.gemini/antigravity-cli/agents/planar-$r.md" ;;
        copilot)     f="$home/.copilot/agents/planar-$r.agent.md" ;;
        *) f="" ;;
      esac
      if [[ -n "$f" ]]; then
        cmp -s "$repo/agents/planar-$r.md" "$f" || fail "$f is not the staged agent"
      fi
    done
    case "$v" in
      codex)
        for r in "${ROLES[@]}"; do
          python3 - "$home/.codex/agents/planar-$r.toml" "planar-$r" <<'PY' || fail "$home/.codex/agents/planar-$r.toml does not parse"
import sys
try:
    import tomllib
except ImportError:
    lines = open(sys.argv[1], encoding='utf-8').read().split('\n')
    assert lines[0] == 'name = "%s"' % sys.argv[2], lines[0]
else:
    d = tomllib.load(open(sys.argv[1], 'rb'))
    assert list(d) == ['name', 'description', 'developer_instructions'], list(d)
    assert d['name'] == sys.argv[2]
PY
        done ;;
      opencode)
        for r in "${ROLES[@]}"; do
          f="$home/.config/opencode/agents/planar-$r.md"
          python3 - "$repo/agents/planar-$r.md" "$f" <<'PY' || fail "$f is not the reduced OpenCode form of planar-$r.md"
import sys
src = open(sys.argv[1], encoding='utf-8').read().split('\n')
out = open(sys.argv[2], encoding='utf-8').read().split('\n')
assert src[0] == '---'
close = src.index('---', 1)
desc = [l for l in src[1:close] if l.startswith('description:')][0]
assert out[:4] == ['---', desc, 'mode: subagent', '---'], out[:4]
assert out[4:] == src[close + 1:], 'body changed'
PY
        done ;;
    esac
  done
}

# assert_manifest HOME -- every placed file and every skill directory is
# recorded in extras, and vendors/projections stay empty.
assert_manifest() {
  local home="$1"
  python3 - "$home" <<'PY' || fail "the manifest does not record every placed path"
import json, os, subprocess, sys
home = sys.argv[1]
m = json.load(open(home + '/.planar/install-manifest.json'))
assert m['vendors'] == [] and m['projections'] == [], (m['vendors'], m['projections'])
extras = set(m['extras'])
out = subprocess.run(['find', '-L', '.', '-type', 'f'], cwd=home, capture_output=True, text=True).stdout.split()
skip = ('./.planar/', './build/', './out', './err', './planar.db', './.gemini/settings.json')
missing = []
for rel in out:
    if rel.startswith(skip):
        continue
    p = home + rel[1:]
    if p not in extras:
        missing.append(p)
    # a skill file's directory is recorded too
    if '/skills/planar/' in p:
        d = p.split('/skills/planar/')[0] + '/skills/planar'
        if d not in extras:
            missing.append(d)
assert not missing, missing[:5]
PY
}

ALL6=(claude codex copilot gemini antigravity opencode)

# 8. All six markers: every target, in the right format.
mk_home h8 "${ALL6[@]}"
run_install "$REPO" "$TMP/h8" || fail "all-vendor install failed: $(cat "$TMP/h8/err")"
assert_found "$TMP/h8" "claude codex copilot gemini antigravity opencode"
assert_placed "$TMP/h8" "${ALL6[@]}"
check_formats "$REPO" "$TMP/h8" "${ALL6[@]}"
[[ ! -e "$TMP/h8/.config/opencode/skills" ]] || fail "a skill was placed in OpenCode's private skill directory"
[[ ! -e "$TMP/h8/.claude/commands" && ! -e "$TMP/h8/.codex/skills" && ! -e "$TMP/h8/.copilot/skills" ]] \
  || fail "an old target (commands, ~/.codex/skills, ~/.copilot/skills) was written"
[[ -z "$(find "$TMP/h8/.gemini/antigravity-cli/skills" -mindepth 1 -maxdepth 1 ! -name planar)" ]] || fail "an old pl-* skill target was written"
[[ "$(find "$TMP/h8/.agents/skills" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ')" == 1 ]] || fail "the shared skill was not placed exactly once"
[[ ! -e "$TMP/h8/.planar/commands" ]] || fail "the render ran"
assert_manifest "$TMP/h8"

# 9. Only ~/.claude/: the Claude skill and agents, nothing else, one vendor found.
mk_home h9 claude
run_install "$REPO" "$TMP/h9" || fail "claude-only install failed: $(cat "$TMP/h9/err")"
assert_found "$TMP/h9" "claude"
assert_placed "$TMP/h9" claude
[[ ! -e "$TMP/h9/.agents" && ! -e "$TMP/h9/.codex" && ! -e "$TMP/h9/.config" && ! -e "$TMP/h9/.gemini" && ! -e "$TMP/h9/.copilot" ]] \
  || fail "a vendor that is not present was written"
grep -Fq 'vendors skipped: codex (no $CODEX_HOME or ~/.codex/), copilot' "$TMP/h9/out" || fail "the skipped vendors were not printed: $(grep skipped "$TMP/h9/out")"
assert_manifest "$TMP/h9"

# 10. Only ~/.config/opencode/: the shared skill and the OpenCode agents.
mk_home h10 opencode
run_install "$REPO" "$TMP/h10" || fail "opencode-only install failed: $(cat "$TMP/h10/err")"
assert_found "$TMP/h10" "opencode"
assert_placed "$TMP/h10" opencode
check_formats "$REPO" "$TMP/h10" opencode
[[ ! -e "$TMP/h10/.config/opencode/skills" && ! -e "$TMP/h10/.claude" ]] || fail "OpenCode install wrote a private skill dir or ~/.claude"

# 11. Gemini CLI and Antigravity are detected independently.
mk_home h11a gemini antigravity
run_install "$REPO" "$TMP/h11a" || fail "gemini+antigravity install failed: $(cat "$TMP/h11a/err")"
assert_found "$TMP/h11a" "gemini antigravity"
assert_placed "$TMP/h11a" gemini antigravity
mk_home h11b gemini            # settings.json alone: no Antigravity path
run_install "$REPO" "$TMP/h11b" || fail "gemini-only install failed: $(cat "$TMP/h11b/err")"
assert_found "$TMP/h11b" "gemini"
assert_placed "$TMP/h11b" gemini
[[ ! -e "$TMP/h11b/.gemini/antigravity-cli" ]] || fail "settings.json alone created an Antigravity path"
mk_home h11c antigravity       # antigravity-cli/ alone: no ~/.gemini/agents
run_install "$REPO" "$TMP/h11c" || fail "antigravity-only install failed: $(cat "$TMP/h11c/err")"
assert_found "$TMP/h11c" "antigravity"
assert_placed "$TMP/h11c" antigravity
[[ ! -e "$TMP/h11c/.gemini/agents" && ! -e "$TMP/h11c/.agents" ]] || fail "antigravity-cli/ alone wrote ~/.gemini/agents or the shared skill"

# 12. Link vs copy: same path set; link mode links skills and Markdown agents;
# the derived OpenCode file is a regular file in both modes.
mk_home h12 "${ALL6[@]}"
run_install "$REPO" "$TMP/h12" --link || fail "link install failed: $(cat "$TMP/h12/err")"
assert_placed "$TMP/h12" "${ALL6[@]}"
check_formats "$REPO" "$TMP/h12" "${ALL6[@]}"
diff <(placed_set "$TMP/h8") <(placed_set "$TMP/h12") >/dev/null || fail "link and copy mode placed different paths"
for p in .claude/skills/planar .agents/skills/planar .gemini/antigravity-cli/skills/planar \
         .claude/agents/planar-coder.md .gemini/agents/planar-coder.md .gemini/antigravity-cli/agents/planar-coder.md \
         .copilot/agents/planar-coder.agent.md .codex/agents/planar-coder.toml; do
  [[ -L "$TMP/h12/$p" ]] || fail "link mode did not link $p"
  [[ "$(readlink "$TMP/h12/$p")" == "$TMP/h12/.planar/"* ]] || fail "$p does not point into the prefix"
done
[[ -f "$TMP/h12/.config/opencode/agents/planar-coder.md" && ! -L "$TMP/h12/.config/opencode/agents/planar-coder.md" ]] \
  || fail "the OpenCode agent is not a regular file in link mode"
[[ -z "$(find "$TMP/h8" -type l ! -path "$TMP/h8/.planar/*")" ]] || fail "copy mode left symlinks outside the prefix"
[[ -f "$TMP/h8/.config/opencode/agents/planar-coder.md" && ! -L "$TMP/h8/.config/opencode/agents/planar-coder.md" ]] \
  || fail "the OpenCode agent is not a regular file in copy mode"
assert_manifest "$TMP/h12"

# 13. A re-install replaces what a prior manifest records, in either mode, and
# the staged prefix is never written through.
run_install "$REPO" "$TMP/h8" --link || fail "copy -> link re-install failed: $(cat "$TMP/h8/err")"
[[ -L "$TMP/h8/.claude/skills/planar" ]] || fail "re-install in link mode did not link the skill"
run_install "$REPO" "$TMP/h8" || fail "link -> copy re-install failed: $(cat "$TMP/h8/err")"
[[ ! -L "$TMP/h8/.claude/skills/planar" && -f "$TMP/h8/.claude/skills/planar/SKILL.md" ]] || fail "re-install in copy mode did not copy the skill"
check_staged "$REPO" "$TMP/h8"
assert_placed "$TMP/h8" "${ALL6[@]}"
assert_manifest "$TMP/h8"

# 14. A foreign destination no manifest records: the installer errs naming the
# path and places nothing, and the foreign file is untouched.
mk_home h14 claude opencode
mkdir -p "$TMP/h14/.claude/skills/planar"
echo foreign > "$TMP/h14/.claude/skills/planar/SKILL.md"
if run_install "$REPO" "$TMP/h14"; then fail "a foreign skill directory did not stop the installer"; fi
grep -Fq "$TMP/h14/.claude/skills/planar" "$TMP/h14/err" || fail "the error does not name the path: $(cat "$TMP/h14/err")"
grep -Fq 'no Planar manifest records it' "$TMP/h14/err" || fail "the error does not say no manifest records it: $(cat "$TMP/h14/err")"
[[ "$(cat "$TMP/h14/.claude/skills/planar/SKILL.md")" == foreign ]] || fail "the foreign file was changed"
[[ "$(placed_set "$TMP/h14")" == "./.claude/skills/planar/SKILL.md" ]] || fail "something was placed despite the foreign destination: $(placed_set "$TMP/h14")"
mk_home h14b claude opencode
mkdir -p "$TMP/h14b/.claude/agents"
echo foreign > "$TMP/h14b/.claude/agents/planar-coder.md"
if run_install "$REPO" "$TMP/h14b"; then fail "a foreign agent file did not stop the installer"; fi
grep -Fq "$TMP/h14b/.claude/agents/planar-coder.md" "$TMP/h14b/err" || fail "the error does not name the agent path"
[[ "$(placed_set "$TMP/h14b")" == "./.claude/agents/planar-coder.md" ]] || fail "something was placed despite the foreign agent: $(placed_set "$TMP/h14b")"
[[ "$(cat "$TMP/h14b/.claude/agents/planar-coder.md")" == foreign ]] || fail "the foreign agent was changed"

# 15. --vendors is a filter: naming an absent vendor warns, an unknown one
# warns, neither is an error; CODEX_HOME set is Codex's marker.
mk_home h15 claude opencode
run_install "$REPO" "$TMP/h15" --vendors claude,codex,bogus || fail "--vendors with an absent vendor failed: $(cat "$TMP/h15/err")"
assert_found "$TMP/h15" "claude"
assert_placed "$TMP/h15" claude
grep -Fq 'names codex but it is not present' "$TMP/h15/err" || fail "no warning for an absent named vendor: $(cat "$TMP/h15/err")"
grep -Fq 'unknown vendor: bogus' "$TMP/h15/err" || fail "no warning for an unknown vendor"
mk_home h15b
RUN_CODEX_HOME="$TMP/h15b/custom-codex" run_install "$REPO" "$TMP/h15b" || fail "CODEX_HOME install failed: $(cat "$TMP/h15b/err")"
assert_found "$TMP/h15b" "codex"
[[ -f "$TMP/h15b/custom-codex/agents/planar-coder.toml" && -f "$TMP/h15b/.agents/skills/planar/SKILL.md" ]] || fail "CODEX_HOME did not select Codex"
[[ ! -e "$TMP/h15b/.codex" ]] || fail "the default ~/.codex was written when CODEX_HOME is set"

# 16. An OpenCode description YAML would misread is quoted, body unchanged.
QUOTE="$TMP/quoterepo"; make_repo "$QUOTE"
printf -- '---\nname: planar-quirky\ndescription: Does a thing: with a colon\nplanar:\n  kind: agent\n  slug: planar-quirky\n---\n\n# Body\n---\nkept\n' > "$QUOTE/agents/planar-quirky.md"
mk_home h16 opencode
run_install "$QUOTE" "$TMP/h16" || fail "quirky install failed: $(cat "$TMP/h16/err")"
[[ "$(sed -n 1,4p "$TMP/h16/.config/opencode/agents/planar-quirky.md" | tr '\n' '|')" == '---|description: "Does a thing: with a colon"|mode: subagent|---|' ]] \
  || fail "the quirky description was not quoted: $(head -5 "$TMP/h16/.config/opencode/agents/planar-quirky.md")"
[[ "$(tail -4 "$TMP/h16/.config/opencode/agents/planar-quirky.md" | tr '\n' '|')" == '|# Body|---|kept|' ]] || fail "the OpenCode body changed"

# 17. Every one of the nine target rows in install.sh carries an adjacent
# documentation URL and the date it was checked.
python3 - "$ROOT/install.sh" <<'PY' || fail "a vendor target row lacks an adjacent doc-URL comment with a date"
import re, sys
lines = open(sys.argv[1], encoding='utf-8').read().split('\n')
row = re.compile(r'^  "[a-z,]+\|(skill|md|toml|copilot|opencode)\|[^"]+"$')
rows = [i for i, l in enumerate(lines) if row.match(l)]
assert len(rows) == 9, len(rows)
for i in rows:
    assert re.match(r'^  # https://\S+.*\(checked \d{4}-\d{2}-\d{2}\)$', lines[i - 1]), (lines[i], lines[i - 1])
text = '\n'.join(lines[i] for i in rows)
assert 'opencode/skills' not in text and '/commands' not in text
PY

printf 'install-stage tests: 17 scenarios passed\n'
