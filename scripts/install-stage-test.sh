#!/usr/bin/env bash
# Installer staging and vendor-surface fixtures (plan 1104, M2): install.sh
# stages skills/planar/ under $PLANAR_HOME, then places the skill into each
# vendor found on the host (six vendors, three skill targets, one presence
# marker each). Role agents are examples and are never staged or placed; an
# upgrade removes the planar-<role> agents an older install placed. Every run is
# against a scratch copy of the installer, a scratch HOME and prefix, and a stub
# `cmake` and stub binaries, so nothing is built and the operator's ~/.planar is
# never looked at.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { printf 'install-stage-test: %s\n' "$*" >&2; exit 1; }
# mark NAME -- the scenario about to run, with the seconds the previous one took.
MARKED=0
mark() { MARKED=$((MARKED + 1)); printf 'stage: scenario %s (previous took %ss)\n' "$1" "$SECONDS"; SECONDS=0; }

# A scratch checkout: the installer, its scripts, the real skill, the agents/
# directory a checkout still carries (which the installer must ignore), and the
# minimum else the installer checks for. The pinned compiler paths are
# pointed at a file that exists everywhere, as install-deps-test.sh points them
# at a missing one.
make_repo() {
  local repo="$1"
  mkdir -p "$repo/skills" "$repo/migrations"
  cp "$ROOT/install.sh" "$repo/install.sh"
  cp -R "$ROOT/scripts" "$repo/scripts"
  cp -R "$ROOT/agents" "$repo/agents"
  cp -R "$ROOT/skills/planar" "$repo/skills/planar"
  cp "$ROOT/install-cleanup.txt" "$repo/install-cleanup.txt"
  : > "$repo/CMakeLists.txt"
  : > "$repo/CMakePresets.json"
  perl -0pi -e 's#/opt/homebrew/opt/llvm/bin/clang(\+\+)?#/bin/sh#g' "$repo/install.sh"
  # A clean git checkout, as a source install has: a re-run after a mid-run
  # failure resumes the interrupted install, and a resume refuses a tree that
  # git cannot show clean (install.sh checkout_clean).
  git -C "$repo" init -q
  git -C "$repo" add -A
  git -C "$repo" -c user.name=t -c user.email=t@example.invalid -c commit.gpgsign=false commit -q -m fixture
}

# Stub cmake and binaries. `cmake --install D --prefix P` puts the stubs in
# P/bin. Only the four binaries are stubbed: the installer builds and installs
# no other executable, and bin/scriptorium from an older install is removed by
# the cleanup manifest.
STUBS="$TMP/stubs"
mkdir -p "$STUBS"
cat > "$STUBS/cmake" <<STUB
#!/usr/bin/env bash
# A stub cmake: \`cmake --install D --prefix P\` writes the four fake binaries of
# scripts/fixtures/prebuilt-bundle.sh (stub_binary_write) into P/bin.
if [[ "\$1" == "--install" ]]; then
  source "$ROOT/scripts/fixtures/prebuilt-bundle.sh"
  mkdir -p "\$4/bin"
  for b in planar planar-agent planar-watch planar-ext; do
    stub_binary_write "\$4/bin/\$b" "\${STUB_TAG:-dev}"
  done
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
  # RUN_SHIM, when set, is a directory searched before everything else (the
  # scenarios use it to put a failing `mv` in front of the installer's).
  env -u CODEX_HOME ${RUN_CODEX_HOME:+CODEX_HOME="$RUN_CODEX_HOME"} \
    PATH="${RUN_SHIM:+$RUN_SHIM:}$STUBS:$PATH" HOME="$home" PLANAR_HOME="$home/.planar" \
    PLANAR_DB="$home/planar.db" NO_COLOR=1 \
    "$repo/install.sh" --build-dir "$home/build" "$@" >"$home/out" 2>"$home/err"
}

# The planar-<role> agents older installs staged and placed in every vendor.
RETIRED_ROLES=(coder ext-sync ingestor janitor orchestrator planner research reviewer spec-reviewer sync-reconciler test-coder)

check_staged() {
  local repo="$1" home="$2" prefix="$2/.planar" f
  [[ -f "$prefix/skills/planar/SKILL.md" ]] || fail "SKILL.md was not staged"
  cmp -s "$repo/skills/planar/SKILL.md" "$prefix/skills/planar/SKILL.md" || fail "staged SKILL.md differs"
  for f in "$repo"/skills/planar/references/*.md; do
    cmp -s "$f" "$prefix/skills/planar/references/$(basename "$f")" || fail "staged $(basename "$f") differs or is missing"
  done
  [[ "$(find "$prefix/skills/planar/" -type f | grep -c .)" == "$(find "$repo/skills/planar/" -type f | grep -c .)" ]] \
    || fail "the staged skill has a different file count than the checkout"
  # No role agent is staged: no agents/, no codex-agents/, whatever the checkout holds.
  [[ ! -e "$prefix/agents" && ! -L "$prefix/agents" ]] || fail "agents/ was staged: $(ls -A "$prefix/agents")"
  [[ ! -e "$prefix/codex-agents" && ! -L "$prefix/codex-agents" ]] || fail "codex-agents/ was staged"
  [[ ! -e "$prefix/skills/src" ]] || fail "the retired skills/src/ tree was staged"
  # The manifest records each staged path, and nothing under agents/ or codex-agents/.
  local m="$prefix/install-manifest.json"
  grep -Fq '"skills/planar/SKILL.md"' "$m" || fail "manifest lacks skills/planar/SKILL.md"
  for f in "$repo"/skills/planar/references/*.md; do
    grep -Fq "\"skills/planar/references/$(basename "$f")\"" "$m" || fail "manifest lacks $(basename "$f")"
  done
  ! grep -Eq '"(agents|codex-agents)/' "$m" || fail "manifest records a staged agent path"
  return 0
}

# Test groups (plan 1122 M5, task 7361). The whole file took 843 s on a loaded host, so ctest
# registers one entry per group (install.stage_<group>, label install_stage_<group>) and no
# entry nears the five-minute ceiling. INSTALL_STAGE_GROUP selects one; unset runs all five.
# An unknown name is a usage error. The helpers and the shared scratch checkout are set up for
# every group; the scenarios are numbered as before and each group's scenarios use only homes
# that group creates.
STAGE_GROUPS="staging vendors placement modes uninstall"
GROUP="${INSTALL_STAGE_GROUP:-all}"
case " all $STAGE_GROUPS " in *" $GROUP "*) ;; *) printf 'install-stage-test: unknown INSTALL_STAGE_GROUP %s (want one of: %s)\n' "$GROUP" "$STAGE_GROUPS" >&2; exit 2 ;; esac
# Scenario selection and parallel dispatch (plan 1122 M6, task 7434; scripts/fixtures/scenario-runner.sh).
# INSTALL_TEST_SCENARIO=<name> runs only that scenario, inside or outside its group; an unknown
# name exits 2. A run that selects several scenarios runs INSTALL_TEST_JOBS of them at a time
# (default 4), each as a child of this script with its own scratch directory and homes. A name
# covers the numbered scenarios that share homes (all-vendors: 8, 9, 12, 13, 23, 24; copy-reinstall:
# 1, 2, 6; mode-switch: 21, 22; upgrade-layout: 26, 30). Scenarios 18 and 29 loop over the copy
# and link modes with homes of their own, so each mode is a name (no-change-copy, no-change-link,
# uninstall-copy, uninstall-link). The table is the dispatch order, name:group. No scenario here
# tests the mutation lock, so none is serial.
SCEN_TABLE="all-vendors:vendors no-change-copy:placement no-change-link:placement uninstall-copy:uninstall uninstall-link:uninstall mode-switch:modes mid-run-failure:placement manifest-rename-failure:placement upgrade-layout:modes copy-reinstall:staging old-agent-layout:staging foreign-destination:staging vendors-filter:staging claude-vendor:staging link-mode:staging checkout-agents-ignored:staging opencode-only:vendors gemini-antigravity:vendors retire-role-agents:modes unprovable-evidence:modes foreign-entries:modes legacy-manifest:modes flags:staging lists:staging doc-urls:staging"
SCEN_SERIAL=""
# shellcheck source=fixtures/scenario-runner.sh
source "$ROOT/scripts/fixtures/scenario-runner.sh"
scen_init install-stage-test "$SCEN_TABLE" "$SCEN_SERIAL"

# The staged scratch checkout is read-only to the scenarios (those that change one make their own
# copy: BAD, QUOTE, MUT), so a parallel child uses the dispatching run's instead of staging another.
if [[ -n "${INSTALL_TEST_SHARED-}" ]]; then REPO="$INSTALL_TEST_SHARED/repo"; else REPO="$TMP/repo"; make_repo "$REPO"; fi
scen_dispatch "$TMP/repo" "$@"

# ---------------------------------------------------------------------------
# `planar health` against a scratch install (task 7219): the installed-surface
# classifier reads the manifest this installer wrote and compares each recorded
# placement with the staged authority. Needs a built planar binary: PLANAR_BIN,
# else build/debug/bin/planar. The scratch HOME, PLANAR_HOME and PLANAR_DB keep
# the run away from the operator's database.
# ---------------------------------------------------------------------------

PLANAR_BIN="${PLANAR_BIN:-$ROOT/build/debug/bin/planar}"
# Without a built planar the health scenarios (22 to 25) are skipped with a printed reason and
# the others still run. ctest registers the install.stage_<group> entries only where the binary is a target.
HEALTH=1
if [[ ! -x "$PLANAR_BIN" ]]; then
  HEALTH=0
  printf 'install-stage tests: health scenarios SKIPPED (no planar binary at %s; set PLANAR_BIN)\n' "$PLANAR_BIN"
fi
# skip_health -- the health scenario about to be left out declares it (scen_skip), once.
skip_health() { scen_skip "scenario $1 needs a built planar (no binary at $PLANAR_BIN; set PLANAR_BIN)"; }

# health_json HOME -- `planar health --json` for a scratch install. The install
# initialized $HOME/planar.db with the stub planar (a placeholder, not SQLite),
# so the real binary gets a database of its own beside it.
health_json() {
  env -u CODEX_HOME HOME="$1" PLANAR_HOME="$1/.planar" PLANAR_DB="$1/planar.db.health" "$PLANAR_BIN" health --json
}

# freshness HOME -- "manifest_status state managed fresh stale missing".
freshness() {
  health_json "$1" | python3 -c '
import json, sys
f = json.load(sys.stdin)["projection_freshness"]
print(f["manifest_status"], f["state"], f["managed"], f["fresh"], f["stale"], f["missing"])'
}


if scen copy-reinstall; then
mark 1
# 1. A fresh copy-mode install, no vendor: the skill is staged, no renderer
# runs, and no agents/ or codex-agents/ is staged.
run_install "$REPO" "$TMP/h1" --no-vendor || fail "install failed: $(cat "$TMP/h1/err")"
check_staged "$REPO" "$TMP/h1"
[[ ! -e "$TMP/h1/.planar/commands" ]] || fail "the installer still runs a renderer"
[[ ! -L "$TMP/h1/.planar/skills/planar" ]] || fail "copy mode staged symlinks"
grep -Fq 'Staging the planar skill' "$TMP/h1/out" || fail "the staging step did not run"
! grep -Fq 'Rendering per-vendor skill outputs' "$TMP/h1/out" || fail "the render section is still in the installer"
[[ ! -e "$TMP/h1/.claude" && ! -e "$TMP/h1/.agents" ]] || fail "--no-vendor placed something in the home directory"

mark 2
# 2. A re-install replaces stale staged files, and removes the agents/ and
# codex-agents/ trees an older install staged (install-cleanup.txt).
echo stale > "$TMP/h1/.planar/skills/planar/references/stale.md"
mkdir -p "$TMP/h1/.planar/agents" "$TMP/h1/.planar/codex-agents"
echo old > "$TMP/h1/.planar/agents/planar-coder.md"; echo old > "$TMP/h1/.planar/agents/methodology.md"
echo old > "$TMP/h1/.planar/codex-agents/planar-coder.toml"
run_install "$REPO" "$TMP/h1" --no-vendor || fail "re-install failed: $(cat "$TMP/h1/err")"
[[ ! -e "$TMP/h1/.planar/skills/planar/references/stale.md" ]] || fail "a re-install kept a stale staged reference"
grep -Fq "removed stale dir  $TMP/h1/.planar/agents/" "$TMP/h1/out" || fail "the cleanup did not report agents/: $(grep removed "$TMP/h1/out")"
grep -Fq "removed stale dir  $TMP/h1/.planar/codex-agents/" "$TMP/h1/out" || fail "the cleanup did not report codex-agents/"
check_staged "$REPO" "$TMP/h1"

fi
if scen claude-vendor; then
mark 3
# 3. With a vendor present and selected the installer stays runnable end to
# end; each placed target is one projection row, its files also in `extras`.
mkdir -p "$TMP/h3/.claude"
run_install "$REPO" "$TMP/h3" --vendors claude || fail "install with --vendors claude failed: $(cat "$TMP/h3/err")"
check_staged "$REPO" "$TMP/h3"
[[ -f "$TMP/h3/.claude/skills/planar/SKILL.md" ]] || fail "the claude vendor was not placed"
[[ ! -e "$TMP/h3/.claude/agents" ]] || fail "a role agent was placed for claude: $(ls -A "$TMP/h3/.claude/agents")"
[[ ! -e "$TMP/h3/.claude/commands" ]] || fail "the old ~/.claude/commands target is still written"
grep -Fq '"vendors": ["claude"]' "$TMP/h3/.planar/install-manifest.json" || fail "the claude placement was not recorded as a vendor"
grep -Fq '"vendor": "claude", "kind": "skill", "name": "planar"' "$TMP/h3/.planar/install-manifest.json" || fail "the claude skill has no projection row"

fi
if scen link-mode; then
mark 4
# 4. Link mode: the staged trees point into the checkout.
run_install "$REPO" "$TMP/h4" --no-vendor --link || fail "link install failed: $(cat "$TMP/h4/err")"
[[ -L "$TMP/h4/.planar/skills/planar" ]] || fail "link mode did not link skills/planar"
check_staged "$REPO" "$TMP/h4"

fi
if scen checkout-agents-ignored; then
mark 5
# 5. The checkout's agents/ is not an install input: a malformed role file there
# (which the retired Codex renderer refused) changes nothing, and nothing from
# agents/ is staged or placed.
BAD="$TMP/badrepo"; make_repo "$BAD"
printf 'no frontmatter at all\n' > "$BAD/agents/planar-broken.md"
git -C "$BAD" add -A; git -C "$BAD" -c user.name=t -c user.email=t@example.invalid -c commit.gpgsign=false commit -q -m broken
mkdir -p "$TMP/h5/.claude"
run_install "$BAD" "$TMP/h5" || fail "a malformed checkout agent stopped the installer: $(cat "$TMP/h5/err")"
check_staged "$BAD" "$TMP/h5"
[[ ! -e "$TMP/h5/.claude/agents" ]] || fail "an agent was placed from the checkout's agents/"
! grep -Fq 'planar-broken' "$TMP/h5/out" "$TMP/h5/err" || fail "the installer read the checkout's agents/"

fi
if scen copy-reinstall; then
mark 6
# 6. The staged recorder, directly: it adds extras only, never projection rows
# (no vendor owns a staged path).
# shellcheck source=install-lib/install-manifest.sh
source "$ROOT/scripts/install-lib/install-manifest.sh"
install_manifest_begin "build-staged" copy
install_manifest_record_staged "$TMP/h1/.planar"
install_manifest_write "$TMP/staged-manifest.json"
grep -Fq '"projections": [' "$TMP/staged-manifest.json" || fail "the manifest lost its projections array"
[[ "${#INSTALL_MANIFEST_ROW_VENDOR[@]}" == 0 ]] || fail "staged paths were recorded as projection rows"
[[ "${#INSTALL_MANIFEST_EXTRAS[@]}" == "$(find "$REPO/skills/planar" -type f | grep -c .)" ]] \
  || fail "unexpected extras count: ${#INSTALL_MANIFEST_EXTRAS[@]}"


fi

# ---------------------------------------------------------------------------
# Vendor surfaces: six vendors, three skill targets, one presence marker each.
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
# The mutation lock directory beside the prefix (.planar.lock) is coordination
# state, not a placed file.
placed_set() {
  (cd "$1" && find -L . -type f \
    ! -path './.planar/*' ! -path './.planar.lock/*' ! -path './build/*' ! -path './out' ! -path './err' \
    ! -path './planar.db*' ! -path './.gemini/settings.json' | sort)
}

SKILL_FILES="$(cd "$REPO/skills/planar" && find . -type f | sed 's#^\./##' | sort)"

# expected_set VENDOR... -- the files the given vendors' targets must hold: the
# skill in each skill root a vendor reads, and no agent file anywhere.
expected_set() {
  local v f shared=0
  for v in "$@"; do
    case "$v" in codex|copilot|gemini|opencode) shared=1 ;; esac
    case "$v" in
      claude)      while read -r f; do echo "./.claude/skills/planar/$f"; done <<< "$SKILL_FILES" ;;
      antigravity) while read -r f; do echo "./.gemini/antigravity-cli/skills/planar/$f"; done <<< "$SKILL_FILES" ;;
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

# check_formats REPO HOME VENDOR... -- each placed skill holds the checkout's
# skill bytes, and no vendor agent directory was created.
check_formats() {
  local repo="$1" home="$2"; shift 2
  local v d
  for v in "$@"; do
    case "$v" in
      claude)      d="$home/.claude/skills/planar" ;;
      antigravity) d="$home/.gemini/antigravity-cli/skills/planar" ;;
      *)           d="$home/.agents/skills/planar" ;;
    esac
    diff -r "$repo/skills/planar" "$d" >/dev/null || fail "$d is not the staged skill"
  done
  for d in .claude/agents .codex/agents .copilot/agents .gemini/agents .gemini/antigravity-cli/agents .config/opencode/agents; do
    [[ ! -e "$home/$d" ]] || fail "a vendor agent directory was created: $d"
  done
}

# assert_manifest HOME [VENDOR...] -- version 2: `vendors` is what the run found
# (all six when none are named), one skill row per placed skill root with the
# right vendor, name, staged source and install kind, no agent row, and every
# placed file and skill directory also in extras.
assert_manifest() {
  local home="$1"; shift
  python3 - "$home" "${@:-claude codex copilot gemini antigravity opencode}" <<'PY' || fail "the manifest does not record every placed path"
import json, os, subprocess, sys
home = sys.argv[1]
found = sys.argv[2].split()
m = json.load(open(home + '/.planar/install-manifest.json'))
assert m['version'] == 2, m['version']
assert m['vendors'] == found, (m['vendors'], found)
extras = set(m['extras'])
out = subprocess.run(['find', '-L', '.', '-type', 'f'], cwd=home, capture_output=True, text=True).stdout.split()
skip = ('./.planar/', './.planar.lock/', './build/', './out', './err', './planar.db', './.gemini/settings.json')
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

# The rows: one skill row per skill root, and no agent row.
want = {}  # installed path -> (vendor label, kind, staged)
skill_root = {'claude': '/.claude/skills', 'antigravity': '/.gemini/antigravity-cli/skills'}
for v in found:
    if v in skill_root:
        want[home + skill_root[v] + '/planar'] = (v, 'skill', home + '/.planar/skills/planar')
    elif v in ('codex', 'copilot', 'gemini', 'opencode'):
        want[home + '/.agents/skills/planar'] = ('shared', 'skill', home + '/.planar/skills/planar')
rows = {r['installed_path']: r for r in m['projections']}
assert len(rows) == len(m['projections']), 'duplicate installed_path'
assert set(rows) == set(want), (sorted(set(rows) ^ set(want))[:5])
for path, (vendor, kind, staged) in want.items():
    r = rows[path]
    assert (r['vendor'], r['kind'], r['staged_path']) == (vendor, kind, staged), (path, r)
    assert r['name'] == 'planar', r
    assert r['install_kind'] == m['install_mode'], r
    assert r['source_digest'] == '' and r['projection_digest'] == '', r
    assert path in extras, path
PY
}

ALL6=(claude codex copilot gemini antigravity opencode)

if scen all-vendors; then
mark 8
# 8. All six markers: every skill target, and no role agent anywhere.
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

mark 9
# 9. Only ~/.claude/: the Claude skill, nothing else, one vendor found.
mk_home h9 claude
run_install "$REPO" "$TMP/h9" || fail "claude-only install failed: $(cat "$TMP/h9/err")"
assert_found "$TMP/h9" "claude"
assert_placed "$TMP/h9" claude
[[ ! -e "$TMP/h9/.agents" && ! -e "$TMP/h9/.codex" && ! -e "$TMP/h9/.config" && ! -e "$TMP/h9/.gemini" && ! -e "$TMP/h9/.copilot" ]] \
  || fail "a vendor that is not present was written"
grep -Fq 'vendors skipped: codex (no $CODEX_HOME or ~/.codex/), copilot' "$TMP/h9/out" || fail "the skipped vendors were not printed: $(grep skipped "$TMP/h9/out")"
assert_manifest "$TMP/h9" claude

fi
if scen opencode-only; then
mark 10
# 10. Only ~/.config/opencode/: the shared skill and no OpenCode agent.
mk_home h10 opencode
run_install "$REPO" "$TMP/h10" || fail "opencode-only install failed: $(cat "$TMP/h10/err")"
assert_found "$TMP/h10" "opencode"
assert_placed "$TMP/h10" opencode
check_formats "$REPO" "$TMP/h10" opencode
[[ ! -e "$TMP/h10/.config/opencode/skills" && ! -e "$TMP/h10/.claude" ]] || fail "OpenCode install wrote a private skill dir or ~/.claude"

fi
if scen gemini-antigravity; then
mark 11
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

fi
if scen all-vendors; then
mark 12
# 12. Link vs copy: same path set; link mode links every skill root.
mk_home h12 "${ALL6[@]}"
run_install "$REPO" "$TMP/h12" --link || fail "link install failed: $(cat "$TMP/h12/err")"
assert_placed "$TMP/h12" "${ALL6[@]}"
check_formats "$REPO" "$TMP/h12" "${ALL6[@]}"
diff <(placed_set "$TMP/h8") <(placed_set "$TMP/h12") >/dev/null || fail "link and copy mode placed different paths"
for p in .claude/skills/planar .agents/skills/planar .gemini/antigravity-cli/skills/planar; do
  [[ -L "$TMP/h12/$p" ]] || fail "link mode did not link $p"
  [[ "$(readlink "$TMP/h12/$p")" == "$TMP/h12/.planar/"* ]] || fail "$p does not point into the prefix"
done
[[ -z "$(find "$TMP/h8" -type l ! -path "$TMP/h8/.planar/*")" ]] || fail "copy mode left symlinks outside the prefix"
assert_manifest "$TMP/h12"

mark 13
# 13. A re-install replaces what a prior manifest records, in either mode, and
# the staged prefix is never written through.
run_install "$REPO" "$TMP/h8" --link || fail "copy -> link re-install failed: $(cat "$TMP/h8/err")"
[[ -L "$TMP/h8/.claude/skills/planar" ]] || fail "re-install in link mode did not link the skill"
run_install "$REPO" "$TMP/h8" || fail "link -> copy re-install failed: $(cat "$TMP/h8/err")"
[[ ! -L "$TMP/h8/.claude/skills/planar" && -f "$TMP/h8/.claude/skills/planar/SKILL.md" ]] || fail "re-install in copy mode did not copy the skill"
check_staged "$REPO" "$TMP/h8"
assert_placed "$TMP/h8" "${ALL6[@]}"
assert_manifest "$TMP/h8"

if [[ "$HEALTH" == 1 ]]; then
mark 23
# 23. Link mode: the installed skill is a symlink into the staged tree, and the
# rows read fresh by construction.
[[ -L "$TMP/h12/.claude/skills/planar" && "$(readlink "$TMP/h12/.claude/skills/planar")" == "$TMP/h12/.planar/skills/planar" ]] \
  || fail "the link-mode skill is not a symlink into the staged tree"
[[ "$(freshness "$TMP/h12")" == "current fresh 3 3 0 0" ]] || fail "link install is not fresh in health: $(freshness "$TMP/h12")"

else
  skip_health 23
fi

if [[ "$HEALTH" == 1 ]]; then
mark 24
# 24. An absent vendor produces no row: only ~/.claude/ left on a host whose
# manifest names all six leaves the one Claude skill root.
for d in .codex .copilot .gemini .config; do rm -rf "${TMP:?}/h8/$d"; done
[[ "$(freshness "$TMP/h8")" == "current fresh 1 1 0 0" ]] || fail "absent vendors still produced rows: $(freshness "$TMP/h8")"
[[ "$(freshness "$TMP/h9")" == "current fresh 1 1 0 0" ]] || fail "the claude-only install is not 1 fresh row: $(freshness "$TMP/h9")"

else
  skip_health 24
fi
fi


if scen foreign-destination; then
mark 14
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
# A planar-coder.md of the operator's own in an agent directory is no longer a
# placement target: the install goes ahead and the file is neither changed,
# removed nor reported (no manifest records it and no staged copy proves it).
mk_home h14b claude opencode
mkdir -p "$TMP/h14b/.claude/agents"
echo foreign > "$TMP/h14b/.claude/agents/planar-coder.md"
run_install "$REPO" "$TMP/h14b" || fail "a foreign agent file stopped the installer: $(cat "$TMP/h14b/err")"
[[ "$(cat "$TMP/h14b/.claude/agents/planar-coder.md")" == foreign ]] || fail "the foreign agent was changed"
! grep -Fq "$TMP/h14b/.claude/agents/planar-coder.md" "$TMP/h14b/out" "$TMP/h14b/err" || fail "the foreign agent was reported"

fi
if scen vendors-filter; then
mark 15
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
[[ -f "$TMP/h15b/.agents/skills/planar/SKILL.md" ]] || fail "CODEX_HOME did not select Codex"
[[ ! -e "$TMP/h15b/custom-codex/agents" ]] || fail "a Codex agent was placed under CODEX_HOME"
[[ ! -e "$TMP/h15b/.codex" ]] || fail "the default ~/.codex was written when CODEX_HOME is set"

fi
if scen doc-urls; then
mark 17
# 17. Every one of the three target rows in install.sh carries an adjacent
# documentation URL and the date it was checked, and every row is a skill.
python3 - "$ROOT/install.sh" <<'PY' || fail "a vendor target row lacks an adjacent doc-URL comment with a date"
import re, sys
lines = open(sys.argv[1], encoding='utf-8').read().split('\n')
row = re.compile(r'^  "[a-z,]+\|(skill|md|toml|copilot|opencode)\|[^"]+"$')
rows = [i for i, l in enumerate(lines) if row.match(l)]
assert len(rows) == 3, len(rows)
assert all('|skill|' in lines[i] for i in rows), [lines[i] for i in rows]
for i in rows:
    assert re.match(r'^  # https://\S+.*\(checked \d{4}-\d{2}-\d{2}\)$', lines[i - 1]), (lines[i], lines[i - 1])
text = '\n'.join(lines[i] for i in rows)
assert 'opencode/skills' not in text and '/commands' not in text and '/agents"' not in text
PY

fi

# ---------------------------------------------------------------------------
# Transactional placement (task 7218): fixed order, the manifest recorded after
# every target through a temp file and a rename, a stop on failure that names
# the target, a re-run that finishes the rest or reports no changes, and a mode
# switch that replaces the prior form.
# ---------------------------------------------------------------------------

NTARGETS=3    # the 3 skill directories, all six vendors

# summary_has HOME TEXT -- the one summary line the placement step prints.
summary_has() { grep -Fq "  $2" "$1/out" || fail "the placement summary lacks '$2': $(grep -E 'vendor target|no changes' "$1/out")"; }

# manifest_abs HOME -- the absolute (placed vendor) paths the manifest records.
manifest_abs() {
  python3 - "$1/.planar/install-manifest.json" <<'PY'
import json, sys
for e in json.load(open(sys.argv[1]))['extras']:
    if e.startswith('/'):
        print(e)
PY
}

if scen no-change-copy || scen no-change-link; then
mark 18
# 18. A second run in the same mode changes nothing: the no-changes summary,
# the manifest byte-identical, and nothing outside the prefix (plus the
# manifest itself) with a newer mtime. The prefix's staged trees are rewritten
# by the staging step on every run, so they are not part of this claim.
for mode in copy link; do
  scen "no-change-$mode" || continue
  flag=(); [[ "$mode" == link ]] && flag=(--link)
  mk_home "h18$mode" "${ALL6[@]}"
  run_install "$REPO" "$TMP/h18$mode" ${flag[@]+"${flag[@]}"} || fail "first $mode install failed: $(cat "$TMP/h18$mode/err")"
  cp "$TMP/h18$mode/.planar/install-manifest.json" "$TMP/m18$mode.json"
  touch "$TMP/stamp18$mode"; sleep 1
  run_install "$REPO" "$TMP/h18$mode" --verbose ${flag[@]+"${flag[@]}"} || fail "second $mode install failed: $(cat "$TMP/h18$mode/err")"
  summary_has "$TMP/h18$mode" "no changes: all $NTARGETS vendor target(s) already match ($mode)"
  [[ "$(grep -c 'unchanged: ' "$TMP/h18$mode/out")" == "$NTARGETS" ]] || fail "$mode: not every target was reported unchanged"
  ! grep -Eq '(placed|replaced): ' "$TMP/h18$mode/out" || fail "$mode: a no-change run placed or replaced something"
  cmp -s "$TMP/m18$mode.json" "$TMP/h18$mode/.planar/install-manifest.json" || fail "$mode: the manifest changed on a no-change run"
  [[ ! "$TMP/h18$mode/.planar/install-manifest.json" -nt "$TMP/stamp18$mode" ]] || fail "$mode: the manifest was rewritten on a no-change run"
  touched="$(find "$TMP/h18$mode" -newer "$TMP/stamp18$mode" ! -path "$TMP/h18$mode/.planar" ! -path "$TMP/h18$mode/.planar/*" \
    ! -path "$TMP/h18$mode/build" ! -path "$TMP/h18$mode/build/*" ! -path "$TMP/h18$mode/out" ! -path "$TMP/h18$mode/err" \
    ! -path "$TMP/h18$mode/planar.db*" ! -path "$TMP/h18$mode/.planar.lock" ! -path "$TMP/h18$mode/.planar.lock/*" ! -path "$TMP/h18$mode")"
  [[ -z "$touched" ]] || fail "$mode: a no-change run modified: $(echo "$touched" | head -5)"
  assert_placed "$TMP/h18$mode" "${ALL6[@]}"
done

fi
if scen mid-run-failure; then
mark 19
# 19. A failure in the middle: the second target (the shared skill) cannot be
# created because a regular file sits where its parent directory must go. The
# pre-check passes it (nothing exists at the destination), placement fails. The
# installer stops naming the target and the reason, the Claude skill before it
# stays placed and recorded, nothing after it exists; once the cause is gone a
# re-run places only the rest.
mk_home h19 "${ALL6[@]}"
: > "$TMP/h19/.agents"
if run_install "$REPO" "$TMP/h19" --verbose; then fail "a mid-run failure did not stop the installer"; fi
grep -Fq "could not place $TMP/h19/.agents/skills/planar" "$TMP/h19/err" || fail "the stop does not name the second target: $(cat "$TMP/h19/err")"
grep -Eq 'File exists|Not a directory' "$TMP/h19/err" || fail "the stop does not give the reason: $(cat "$TMP/h19/err")"
[[ -f "$TMP/h19/.claude/skills/planar/SKILL.md" ]] || fail "the target before the failure was not placed"
[[ "$(grep -c 'placed: ' "$TMP/h19/out")" == 1 ]] || fail "expected exactly one placed target before the stop: $(grep 'placed: ' "$TMP/h19/out")"
[[ ! -e "$TMP/h19/.gemini/antigravity-cli/skills/planar" ]] || fail "a target after the failure was placed"
[[ -f "$TMP/h19/.agents" && ! -d "$TMP/h19/.agents" ]] || fail "the blocker file was disturbed"
python3 - "$TMP/h19" <<'PY' || fail "the manifest after the stop does not record exactly the one placed target"
import json, sys
h = sys.argv[1]
m = json.load(open(h + '/.planar/install-manifest.json'))
absolute = sorted(e for e in m['extras'] if e.startswith('/'))
root = h + '/.claude/skills/planar'
assert root in absolute, root
assert all(e == root or e.startswith(root + '/') for e in absolute), absolute
PY
rm -f "$TMP/h19/.agents"
run_install "$REPO" "$TMP/h19" --verbose || fail "the re-run after the fix failed: $(cat "$TMP/h19/err")"
summary_has "$TMP/h19" "placed $((NTARGETS - 1)), replaced 0, unchanged 1 of $NTARGETS vendor target(s) (copy)"
[[ "$(grep -c 'unchanged: ' "$TMP/h19/out")" == 1 && "$(grep -c 'placed: ' "$TMP/h19/out")" == $((NTARGETS - 1)) ]] || fail "the re-run did not report only the remainder as changed"
assert_placed "$TMP/h19" "${ALL6[@]}"
check_formats "$REPO" "$TMP/h19" "${ALL6[@]}"
assert_manifest "$TMP/h19"

fi
if scen manifest-rename-failure; then
mark 20
# 20. A crash between the manifest's temp write and its rename. A `mv` shim in
# front of the installer's PATH fails the third rename of install-manifest.json
# (the first is the manifest written before any target, the second follows the
# first target, the third would follow the second). The manifest on disk is the
# previous complete one and parses; no temp file is left; the second target is
# on disk but unrecorded, and a re-run completes without refusing it.
SHIM="$TMP/shim"; mkdir -p "$SHIM"
cat > "$SHIM/mv" <<'SHIMEOF'
#!/usr/bin/env bash
for a in "$@"; do
  case "$a" in
    *install-manifest.json)
      n="$(cat "$SHIM_COUNT" 2>/dev/null || echo 0)"; n=$((n + 1)); echo "$n" > "$SHIM_COUNT"
      if [[ "$n" -eq "$SHIM_FAIL_AT" ]]; then echo "mv: simulated rename failure" >&2; exit 1; fi ;;
  esac
done
exec /bin/mv "$@"
SHIMEOF
chmod +x "$SHIM/mv"
mk_home h20 "${ALL6[@]}"
if RUN_SHIM="$SHIM" SHIM_COUNT="$TMP/shim-count" SHIM_FAIL_AT=3 run_install "$REPO" "$TMP/h20"; then fail "a failed manifest rename did not stop the installer"; fi
[[ "$(cat "$TMP/shim-count")" == 3 ]] || fail "the shim did not see exactly three manifest renames: $(cat "$TMP/shim-count")"
grep -Fq 'previous manifest is intact' "$TMP/h20/err" || fail "the rename failure was not reported: $(cat "$TMP/h20/err")"
[[ -z "$(find "$TMP/h20/.planar" -name 'install-manifest.json.tmp.*')" ]] || fail "a manifest temp file was left behind"
python3 - "$TMP/h20" <<'PY' || fail "the manifest after a failed rename is not the previous complete version"
import json, sys
h = sys.argv[1]
m = json.load(open(h + '/.planar/install-manifest.json'))
absolute = [e for e in m['extras'] if e.startswith('/')]
assert absolute and all(e == h + '/.claude/skills/planar' or e.startswith(h + '/.claude/skills/planar/') for e in absolute), absolute
PY
[[ -f "$TMP/h20/.agents/skills/planar/SKILL.md" ]] || fail "the second target should be on disk, unrecorded"
run_install "$REPO" "$TMP/h20" --verbose || fail "the re-run after a failed rename refused or failed: $(cat "$TMP/h20/err")"
summary_has "$TMP/h20" "placed $((NTARGETS - 2)), replaced 0, unchanged 2 of $NTARGETS vendor target(s) (copy)"
assert_placed "$TMP/h20" "${ALL6[@]}"
assert_manifest "$TMP/h20"

fi

if scen mode-switch; then
mark 21
# 21. Switching mode replaces the prior form of every target and records the
# new mode; the path set is identical.
mk_home h21 "${ALL6[@]}"
run_install "$REPO" "$TMP/h21" || fail "copy install failed: $(cat "$TMP/h21/err")"
placed_set "$TMP/h21" > "$TMP/set21-copy"
grep -Fq '"install_mode": "copy"' "$TMP/h21/.planar/install-manifest.json" || fail "the manifest does not record copy mode"
run_install "$REPO" "$TMP/h21" --link || fail "copy -> link switch failed: $(cat "$TMP/h21/err")"
summary_has "$TMP/h21" "placed 0, replaced $NTARGETS, unchanged 0 of $NTARGETS vendor target(s) (link)"
placed_set "$TMP/h21" > "$TMP/set21-link"
cmp -s "$TMP/set21-copy" "$TMP/set21-link" || fail "the path set changed on the switch to link mode"
grep -Fq '"install_mode": "link"' "$TMP/h21/.planar/install-manifest.json" || fail "the manifest still records copy mode after the switch"
[[ -z "$(find "$TMP/h21/.claude" "$TMP/h21/.codex" "$TMP/h21/.copilot" "$TMP/h21/.gemini" "$TMP/h21/.agents" -type f ! -name settings.json)" ]] \
  || fail "link mode left a regular file where a link belongs"
[[ -L "$TMP/h21/.agents/skills/planar" && -L "$TMP/h21/.claude/skills/planar" ]] || fail "the switch to link did not link"
run_install "$REPO" "$TMP/h21" || fail "link -> copy switch failed: $(cat "$TMP/h21/err")"
summary_has "$TMP/h21" "placed 0, replaced $NTARGETS, unchanged 0 of $NTARGETS vendor target(s) (copy)"
placed_set "$TMP/h21" | cmp -s - "$TMP/set21-copy" || fail "the path set changed on the switch back to copy"
grep -Fq '"install_mode": "copy"' "$TMP/h21/.planar/install-manifest.json" || fail "the manifest still records link mode after the switch back"
[[ -z "$(find "$TMP/h21/.claude" "$TMP/h21/.codex" "$TMP/h21/.copilot" "$TMP/h21/.gemini" "$TMP/h21/.agents" -type l)" ]] \
  || fail "copy mode left a symlink where a copy belongs"
assert_manifest "$TMP/h21"
check_formats "$REPO" "$TMP/h21" "${ALL6[@]}"

fi

if scen mode-switch; then
if [[ "$HEALTH" == 1 ]]; then
mark 22
# 22. A copy install across all six vendors: three skill roots, every one
# fresh. One byte changed in the installed Claude
# skill flips exactly that row to stale and degrades health; restoring it
# clears it.
[[ "$(freshness "$TMP/h21")" == "current fresh 3 3 0 0" ]] || fail "all-vendor copy install is not fresh in health: $(freshness "$TMP/h21")"
SKILLMD="$TMP/h21/.claude/skills/planar/SKILL.md"
cp "$SKILLMD" "$TMP/skill.bak"
printf 'x' >> "$SKILLMD"
[[ "$(freshness "$TMP/h21")" == "current degraded 3 2 1 0" ]] || fail "one drifted byte did not flip exactly one row: $(freshness "$TMP/h21")"
cp "$TMP/skill.bak" "$SKILLMD"
[[ "$(freshness "$TMP/h21")" == "current fresh 3 3 0 0" ]] || fail "restoring the byte did not clear the drift: $(freshness "$TMP/h21")"
rm -rf "$TMP/h21/.agents/skills/planar"
[[ "$(freshness "$TMP/h21")" == "current degraded 3 2 0 1" ]] || fail "a removed skill directory is not one missing row: $(freshness "$TMP/h21")"

else
  skip_health 22
fi
fi

if scen legacy-manifest; then
if [[ "$HEALTH" == 1 ]]; then
mark 25
# 25. A manifest in the previous shape (version 1: the four retired vendors, or
# the interim extras-only layout) is one degraded `legacy` contributor with no
# rows; health still answers.
mkdir -p "$TMP/hold/.planar"
printf '{"version": 1, "build_id": "old", "install_mode": "copy", "vendors": ["claude"], "extras": ["skills/planar/SKILL.md"], "projections": []}\n' \
  > "$TMP/hold/.planar/install-manifest.json"
[[ "$(freshness "$TMP/hold")" == "legacy degraded 0 0 0 0" ]] || fail "a version 1 manifest is not one degraded legacy state: $(freshness "$TMP/hold")"
printf '{"version": 1, "build_id": "old", "install_mode": "copy", "vendors": ["claude"], "projections": [{"vendor": "claude", "kind": "skill", "name": "pl-x", "staged_path": "/s", "installed_path": "/i", "install_kind": "copy", "source_digest": "", "projection_digest": ""}]}\n' \
  > "$TMP/hold/.planar/install-manifest.json"
[[ "$(freshness "$TMP/hold")" == "legacy degraded 0 0 0 0" ]] || fail "a version 1 manifest with old rows is not legacy: $(freshness "$TMP/hold")"

else
  skip_health 25
fi
fi


# ---------------------------------------------------------------------------
# Retiring the previous projections (task 7223): the sweep removes only what it
# can prove Planar made, before the $PLANAR_HOME cleanup deletes the staged
# trees that are the proof; the cleanup list names every retired prefix path;
# --uninstall removes the new targets and nothing else.
# ---------------------------------------------------------------------------

if scen upgrade-layout; then
mark 26
# 26. Upgrade from the previous layout: seven artifacts, all retired and
# reported; the sweep title comes before the cleanup output; the staged
# *-skills/ trees are gone afterwards, and so are agents/ and codex-agents/.
H="$TMP/h26"; P="$H/.planar"
mk_home h26 "${ALL6[@]}"
mkdir -p "$P/commands/claude" "$P/codex-skills/pl-task/references" "$P/opencode-skills/pl-task" \
  "$P/copilot-skills/pl-task" "$P/gemini-skills/pl-task" "$P/agents/claude" "$P/agents/opencode" "$P/agents/codex" "$P/bin"
touch "$P/.planar-install"
echo cmd > "$P/commands/claude/pl-task.md"
echo skill > "$P/codex-skills/pl-task/SKILL.md"; echo ref > "$P/codex-skills/pl-task/references/r.md"
echo oskill > "$P/opencode-skills/pl-task/SKILL.md"
echo c > "$P/copilot-skills/pl-task/SKILL.md"; echo g > "$P/gemini-skills/pl-task/SKILL.md"
echo agent > "$P/agents/claude/coder.md"; echo toml > "$P/agents/codex/coder.toml"
printf '#!/bin/sh\necho old\n' > "$P/bin/scriptorium"; chmod +x "$P/bin/scriptorium"
mkdir -p "$H/.claude/commands" "$H/.codex/skills" "$H/.copilot/skills" "$H/.gemini/antigravity-cli/skills" \
  "$H/.config/opencode/skills" "$H/.claude/agents" "$H/.gemini/agents" "$H/.gemini/antigravity-cli/agents" "$H/.codex/agents"
ln -s "$P/commands/claude/pl-task.md" "$H/.claude/commands/pl-task.md"
cp -R "$P/codex-skills/pl-task" "$H/.codex/skills/pl-task"
cp -R "$P/opencode-skills/pl-task" "$H/.config/opencode/skills/pl-task"
cp -R "$P/copilot-skills/pl-task" "$H/.copilot/skills/pl-task"
cp -R "$P/gemini-skills/pl-task" "$H/.gemini/antigravity-cli/skills/pl-task"
ln -s "$P/agents/claude/coder.md" "$H/.claude/agents/coder.md"
mkdir -p "$H/.config/opencode/agents"
ln -s "$P/agents/opencode/coder.md" "$H/.config/opencode/agents/coder.md"   # dangling: no such file is staged
ln -s "$P/agents/codex/coder.toml" "$H/.codex/agents/coder.toml"
[[ -L "$H/.config/opencode/agents/coder.md" && ! -e "$H/.config/opencode/agents/coder.md" ]] || fail "the dangling fixture is not dangling"
run_install "$REPO" "$H" || fail "upgrade install failed: $(cat "$H/err")"
for gone in "$H/.claude/commands/pl-task.md" "$H/.codex/skills/pl-task" "$H/.claude/agents/coder.md" \
            "$H/.config/opencode/skills/pl-task" "$H/.config/opencode/agents/coder.md" "$H/.codex/agents/coder.toml" \
            "$H/.copilot/skills/pl-task" "$H/.gemini/antigravity-cli/skills/pl-task"; do
  [[ ! -e "$gone" && ! -L "$gone" ]] || fail "the upgrade left $gone"
  grep -Fq "$gone" "$H/out" || fail "the removal of $gone was not printed: $(cat "$H/out")"
done
[[ ! -e "$P/agents/codex" && ! -e "$P/agents/claude" && ! -e "$P/agents/opencode" ]] || fail "agents/<vendor>/ survived the upgrade"
grep -Fq "removed stale dir  $P/agents/codex/" "$H/out" || grep -Fq "removed stale dir  $P/agents/codex" "$H/out" || fail "the cleanup did not report agents/codex/"
for t in commands codex-skills copilot-skills gemini-skills opencode-skills; do
  [[ ! -e "$P/$t" ]] || fail "the staged $t survived the cleanup"
done
# The renderer is no longer built, so the cleanup removes the seeded stale binary.
[[ ! -e "$P/bin/scriptorium" ]] || fail "the stale bin/scriptorium survived the cleanup"
grep -Fq "removed stale file $P/bin/scriptorium" "$H/out" || fail "the removal of bin/scriptorium was not printed: $(cat "$H/out")"
[[ -x "$P/bin/planar" ]] || fail "the cleanup removed the freshly installed bin/planar"
sweep_line="$(grep -n 'Retiring the previous skill and agent projections' "$H/out" | head -1 | cut -d: -f1)"
clean_line="$(grep -n 'removed stale' "$H/out" | head -1 | cut -d: -f1)"
[[ -n "$sweep_line" && -n "$clean_line" && "$sweep_line" -lt "$clean_line" ]] || fail "the sweep does not precede the cleanup output ($sweep_line, $clean_line)"
for t in commands/ skills/codex/ skills/copilot/ skills/gemini/ codex-skills/ copilot-skills/ gemini-skills/ opencode-skills/ \
         agents/claude/ agents/codex/ agents/copilot/ agents/gemini/ agents/opencode/ bin/scriptorium agents/ codex-agents/; do
  grep -Eq "^$t([[:space:]]|\$)" "$ROOT/install-cleanup.txt" || fail "install-cleanup.txt does not list $t"
done
check_staged "$REPO" "$H"
grep -Fq 'retired 8 previous projection(s); left 0' "$H/out" || fail "unexpected sweep summary: $(grep 'previous projection' "$H/out")"

mark 30
# 30. A second run after the upgrade removes nothing and reports nothing left.
run_install "$REPO" "$H" || fail "re-run after the upgrade failed: $(cat "$H/err")"
! grep -Eq 'removed (stale|command|agent|skill)' "$H/out" || fail "the re-run removed something: $(grep removed "$H/out")"
grep -Fq 'retired 0 previous projection(s); left 0' "$H/out" || fail "the re-run sweep summary: $(grep 'previous projection' "$H/out")"
grep -Fq 'no changes: all' "$H/out" || fail "the re-run changed vendor targets: $(grep -E 'vendor target|no changes' "$H/out")"

fi
if scen unprovable-evidence; then
mark 27
# 27. Missing or differing evidence: the candidate stays, with its path and why.
H="$TMP/h27"; P="$H/.planar"
mk_home h27 codex opencode
mkdir -p "$H/.config/opencode/skills/pl-task" "$H/.codex/skills/pl-task" "$P/codex-skills/pl-task"
echo mine > "$H/.config/opencode/skills/pl-task/SKILL.md"
echo staged > "$P/codex-skills/pl-task/SKILL.md"; echo different > "$H/.codex/skills/pl-task/SKILL.md"
touch "$P/.planar-install"
run_install "$REPO" "$H" || fail "install with unprovable candidates failed: $(cat "$H/err")"
[[ -f "$H/.config/opencode/skills/pl-task/SKILL.md" && -f "$H/.codex/skills/pl-task/SKILL.md" ]] || fail "an unprovable candidate was removed"
[[ "$(grep -c 'could not prove ownership' "$H/err")" == 2 ]] || fail "expected two could-not-prove lines: $(cat "$H/err")"
grep -Fq "$H/.config/opencode/skills/pl-task: could not prove ownership" "$H/err" || fail "the opencode candidate is not reported with its path"
grep -Fq "$H/.codex/skills/pl-task: could not prove ownership" "$H/err" || fail "the codex candidate is not reported with its path"
grep -Fq 'retired 0 previous projection(s); left 2' "$H/out" || fail "the sweep summary does not count the two left"

fi
if scen foreign-entries; then
mark 28
# 28. Foreign entries are never touched and never removed.
H="$TMP/h28"
mk_home h28 claude codex opencode
mkdir -p "$H/.claude/commands" "$H/.codex/skills/pl-other" "$H/.config/opencode/agents"
echo mine > "$H/.claude/commands/pl-mine.md"; echo other > "$H/.codex/skills/pl-other/SKILL.md"
echo mine > "$H/.config/opencode/agents/mine.md"
echo local > "$H/.claude/commands/local-mine.md"
ln -s /somewhere/else "$H/.claude/commands/pl-elsewhere.md"
run_install "$REPO" "$H" || fail "install with foreign entries failed: $(cat "$H/err")"
for keep in "$H/.claude/commands/pl-mine.md" "$H/.codex/skills/pl-other/SKILL.md" "$H/.config/opencode/agents/mine.md" \
            "$H/.claude/commands/local-mine.md"; do
  [[ -f "$keep" ]] || fail "a foreign entry was removed: $keep"
done
[[ -L "$H/.claude/commands/pl-elsewhere.md" ]] || fail "a symlink outside the prefix was removed"
! grep -Eq 'removed (command|agent|skill)' "$H/out" || fail "the installer printed a removal for a foreign entry: $(grep removed "$H/out")"

fi

if scen uninstall-copy || scen uninstall-link; then
mark 29
# 29. --uninstall: every recorded target and the prefix contents go; the vendor
# directories, an unrecorded foreign file and a recorded target that was
# replaced by someone else stay, and the leftovers are reported.
for mode in copy link; do
  scen "uninstall-$mode" || continue
  flag=(); [[ "$mode" == link ]] && flag=(--link)
  H="$TMP/h29$mode"; P="$H/.planar"
  mk_home "h29$mode" "${ALL6[@]}"
  run_install "$REPO" "$H" ${flag[@]+"${flag[@]}"} || fail "$mode: install before uninstall failed: $(cat "$H/err")"
  manifest_abs "$H" > "$TMP/recorded29$mode"
  [[ -s "$TMP/recorded29$mode" ]] || fail "$mode: nothing recorded"
  mkdir -p "$H/.claude/agents"; echo mine > "$H/.claude/agents/planar-mine.md"
  R="$H/.gemini/antigravity-cli/skills/planar"
  rm -rf "$R"; mkdir -p "$R"; echo replaced > "$R/SKILL.md"
  run_install "$REPO" "$H" --uninstall || fail "$mode: uninstall failed: $(cat "$H/err")"
  while IFS= read -r p; do
    case "$p" in "$R"|"$R"/*) continue ;; esac
    [[ ! -e "$p" && ! -L "$p" ]] || fail "$mode: uninstall left the recorded target $p"
  done < "$TMP/recorded29$mode"
  [[ "$(cat "$R/SKILL.md")" == replaced ]] || fail "$mode: a replaced target was removed"
  grep -Fq "$R" "$H/err" || fail "$mode: the replaced target was not reported"
  [[ -f "$H/.claude/agents/planar-mine.md" ]] || fail "$mode: the foreign file was removed"
  grep -Fq "$H/.claude/agents/planar-mine.md" "$H/err" || fail "$mode: the foreign file was not reported"
  for d in .claude/skills .claude/agents .agents/skills .gemini/antigravity-cli/skills; do
    [[ -d "$H/$d" ]] || fail "$mode: uninstall removed the vendor directory $d"
  done
  [[ ! -e "$P/bin" && ! -e "$P/install-manifest.json" && ! -e "$P/skills" ]] || fail "$mode: the prefix contents survived: $(ls -A "$P")"
done

fi

if scen old-agent-layout; then
mark 31
# 31. Upgrade from the older top-level agent layout. An older release linked
# vendor agents straight to $PLANAR_HOME/agents/<role>.md (no <vendor>/ level).
# The upgrade rebuilds that directory with planar-*.md only, so those links
# would dangle. The sweep removes every such link, live or already dangling,
# and leaves a foreign link, a regular file and a link elsewhere under the
# prefix. A link-mode install followed by a re-run keeps its own skill links,
# so no rule sweeps the current layout.
H="$TMP/h31"; P="$H/.planar"
mk_home h31 claude codex
mkdir -p "$P/agents" "$H/.codex/agents" "$H/.claude/agents" "$P/scripts"
touch "$P/.planar-install"
echo old > "$P/agents/coder.md"; echo old > "$P/agents/methodology.md"
ln -s "$P/agents/coder.md" "$H/.codex/agents/coder.md"                   # live, retired layout
ln -s "$P/agents/methodology.md" "$H/.claude/agents/methodology.md"       # live, retired layout
ln -s "$P/agents/doc-author.md" "$H/.codex/agents/doc-author.md"         # already dangling
ln -s /somewhere/else/coder.md "$H/.codex/agents/foreign.md"             # foreign link
echo mine > "$H/.codex/agents/mine.md"                                   # regular file
echo s > "$P/scripts/x.md"; ln -s "$P/scripts/x.md" "$H/.claude/agents/x.md"   # under the prefix, not agents/
mkdir -p "$P/agents/notes"; echo n > "$P/agents/notes/n.md"
ln -s "$P/agents/notes/n.md" "$H/.claude/agents/n.md"                    # a subdirectory no release used
[[ -L "$H/.codex/agents/doc-author.md" && ! -e "$H/.codex/agents/doc-author.md" ]] || fail "the dangling fixture is not dangling"
run_install "$REPO" "$H" || fail "upgrade from the top-level agent layout failed: $(cat "$H/err")"
for gone in "$H/.codex/agents/coder.md" "$H/.claude/agents/methodology.md" "$H/.codex/agents/doc-author.md"; do
  [[ ! -e "$gone" && ! -L "$gone" ]] || fail "the upgrade left the retired top-level agent link $gone"
  grep -Fq "removed agent symlink: $gone" "$H/out" || fail "the removal of $gone was not printed: $(grep -i removed "$H/out")"
done
[[ -L "$H/.codex/agents/foreign.md" ]] || fail "a foreign agent link was removed"
[[ -f "$H/.codex/agents/mine.md" ]] || fail "a regular agent file was removed"
[[ -L "$H/.claude/agents/x.md" ]] || fail "a link elsewhere under the prefix was removed"
[[ -L "$H/.claude/agents/n.md" ]] || fail "a link into an unknown agents/ subdirectory was removed"
grep -Fq 'retired 3 previous projection(s); left 0' "$H/out" || fail "unexpected sweep summary: $(grep 'previous projection' "$H/out")"
H="$TMP/h31link"
mk_home h31link "${ALL6[@]}"
run_install "$REPO" "$H" --link || fail "link-mode install failed: $(cat "$H/err")"
[[ -L "$H/.claude/skills/planar" ]] || fail "link mode did not place the skill as a link"
run_install "$REPO" "$H" --link || fail "link-mode re-run failed: $(cat "$H/err")"
grep -Fq 'retired 0 previous projection(s); left 0' "$H/out" || fail "the link-mode re-run swept its own links: $(grep -E 'removed|previous projection' "$H/out")"
[[ -L "$H/.claude/skills/planar" ]] || fail "the link-mode re-run lost its skill link"


fi
if scen retire-role-agents; then
mark 34
# 34. Upgrade from an install that placed role agents. An older install staged
# $PLANAR_HOME/agents/planar-<role>.md and codex-agents/planar-<role>.toml and
# placed every role into all six vendors' agent directories: Markdown for
# Claude Code, Gemini CLI and Antigravity, planar-<role>.agent.md for Copilot,
# the derived form for OpenCode and the TOML for Codex. Once in copy mode and
# once in link mode (a dangling link too), the upgrade leaves no role agent in
# any vendor directory and no agents/ or codex-agents/ under $PLANAR_HOME. It
# never touches a planar-local-* agent (`planar local link`), an operator's own
# planar-* file, or a file outside the planar-* names; a copy Planar placed and
# the operator then edited is left and reported. A second run removes nothing.
# shellcheck source=install-lib/ownership.sh
( source "$ROOT/scripts/install-lib/ownership.sh"; declare -f opencode_derive ) > "$TMP/derive.sh"
# shellcheck disable=SC1091
source "$TMP/derive.sh"
# seed_old_install HOME MODE -- the prefix and vendor directories an install
# that placed role agents left behind, in MODE (copy or link).
seed_old_install() {
  local H="$1" mode="$2" P="$1/.planar" r d
  mk_home "${H##*/}" "${ALL6[@]}"
  mkdir -p "$P/agents" "$P/codex-agents" "$P/local/agents"
  touch "$P/.planar-install"
  echo doctrine > "$P/agents/methodology.md"
  for r in "${RETIRED_ROLES[@]}"; do
    printf -- '---\nname: planar-%s\ndescription: The %s role: a retired example.\n---\n\nBody of %s.\n' "$r" "$r" "$r" > "$P/agents/planar-$r.md"
    printf 'name = "planar-%s"\ndescription = "The %s role"\ndeveloper_instructions = "Body"\n' "$r" "$r" > "$P/codex-agents/planar-$r.toml"
  done
  for d in .claude/agents .codex/agents .copilot/agents .gemini/agents .gemini/antigravity-cli/agents .config/opencode/agents; do
    mkdir -p "$H/$d"
  done
  for r in "${RETIRED_ROLES[@]}"; do
    opencode_derive "$P/agents/planar-$r.md" > "$H/.config/opencode/agents/planar-$r.md"
    if [[ "$mode" == link ]]; then
      for d in .claude/agents .gemini/agents .gemini/antigravity-cli/agents; do ln -s "$P/agents/planar-$r.md" "$H/$d/planar-$r.md"; done
      ln -s "$P/agents/planar-$r.md" "$H/.copilot/agents/planar-$r.agent.md"
      ln -s "$P/codex-agents/planar-$r.toml" "$H/.codex/agents/planar-$r.toml"
    else
      for d in .claude/agents .gemini/agents .gemini/antigravity-cli/agents; do cp "$P/agents/planar-$r.md" "$H/$d/planar-$r.md"; done
      cp "$P/agents/planar-$r.md" "$H/.copilot/agents/planar-$r.agent.md"
      cp "$P/codex-agents/planar-$r.toml" "$H/.codex/agents/planar-$r.toml"
    fi
  done
  # Kept: operator-local agents (a link into local/ and a regular file), the
  # operator's own planar-* file, a non-planar file, and a planar-* link that
  # points outside the staged trees.
  echo helper > "$P/local/agents/helper.md"
  ln -s "$P/local/agents/helper.md" "$H/.claude/agents/planar-local-helper.md"
  echo 'name = "planar-local-helper"' > "$H/.codex/agents/planar-local-helper.toml"
  # A planar-local-* name is never the sweep's even where the staged-tree proof
  # would hold: a link into the old agents/ tree, beside a staged file of that name.
  echo pinned > "$P/agents/planar-local-pinned.md"
  ln -s "$P/agents/planar-local-pinned.md" "$H/.gemini/antigravity-cli/agents/planar-local-pinned.md"
  echo mine > "$H/.gemini/agents/planar-mine.md"
  echo mine > "$H/.claude/agents/notes.md"
  ln -s /somewhere/else/planar-x.md "$H/.copilot/agents/planar-x.agent.md"
}
for mode in copy link; do
  H="$TMP/h34$mode"; P="$H/.planar"
  seed_old_install "$H" "$mode"
  # Copies an older install placed, which the operator then edited: one per
  # proof form (the derived OpenCode copy, a Markdown copy, the Copilot
  # .agent.md copy, the Codex TOML). Each becomes a regular file holding the
  # placed bytes plus an edit, so neither a link nor a byte match proves it.
  # Three are in the old manifest and are reported; the Copilot one is not
  # recorded, so it is not Planar's to report and is left silently.
  edited_oc="$H/.config/opencode/agents/planar-coder.md"
  edited_md="$H/.claude/agents/planar-coder.md"
  edited_cp="$H/.copilot/agents/planar-coder.agent.md"
  edited_cx="$H/.codex/agents/planar-coder.toml"
  for e in "$edited_oc" "$edited_md" "$edited_cp" "$edited_cx"; do
    { cat "$e"; printf '\nmy edit\n'; } > "$e.tmp"
    rm -f "$e"; mv "$e.tmp" "$e"
  done
  (
    source "$ROOT/scripts/install-lib/install-manifest.sh"
    install_manifest_begin "old-build" "$mode"
    INSTALL_MANIFEST_VENDORS=("${ALL6[@]}")
    install_manifest_add opencode agent planar-coder "$P/agents/planar-coder.md" "$edited_oc" copy
    install_manifest_add claude agent planar-coder "$P/agents/planar-coder.md" "$edited_md" "$mode"
    install_manifest_add codex agent planar-coder "$P/codex-agents/planar-coder.toml" "$edited_cx" "$mode"
    install_manifest_write "$P/install-manifest.json"
  ) || fail "$mode: could not write the old manifest"
  if [[ "$mode" == link ]]; then
    ln -s "$P/agents/planar-gone.md" "$H/.claude/agents/planar-gone.md"   # dangling: an older role
  fi
  flag=(); [[ "$mode" == link ]] && flag=(--link)
  run_install "$REPO" "$H" ${flag[@]+"${flag[@]}"} || fail "$mode: upgrade from placed role agents failed: $(cat "$H/err")"
  left="$(find "$H/.claude/agents" "$H/.codex/agents" "$H/.copilot/agents" "$H/.gemini/agents" \
    "$H/.gemini/antigravity-cli/agents" "$H/.config/opencode/agents" -name 'planar-*' ! -name 'planar-local-*' \
    ! -path "$edited_oc" ! -path "$edited_md" ! -path "$edited_cp" ! -path "$edited_cx" \
    ! -name 'planar-mine.md' ! -name 'planar-x.agent.md' | sort)"
  [[ -z "$left" ]] || fail "$mode: the upgrade left role agents behind: $left"
  [[ ! -e "$P/agents" && ! -e "$P/codex-agents" ]] || fail "$mode: agents/ or codex-agents/ survived the upgrade"
  for r in "${RETIRED_ROLES[@]}"; do
    [[ "$r" == coder ]] && continue   # the edited Codex copy stays
    grep -Fq "removed role agent: $H/.codex/agents/planar-$r.toml" "$H/out" || fail "$mode: the removal of the planar-$r Codex agent was not printed"
  done
  n=$(( ${#RETIRED_ROLES[@]} * 6 - 4 ))
  [[ "$mode" == link ]] && n=$((n + 1))
  for e in "$edited_oc" "$edited_md" "$edited_cp" "$edited_cx"; do
    [[ -f "$e" && ! -L "$e" && "$(tail -1 "$e")" == "my edit" ]] || fail "$mode: the edited copy $e was removed or changed"
  done
  grep -Fq "retired $n previous projection(s); left 3" "$H/out" || fail "$mode: unexpected sweep summary: $(grep 'previous projection' "$H/out")"
  for e in "$edited_oc" "$edited_md" "$edited_cx"; do
    grep -Fq "left $e: Planar placed it" "$H/err" || fail "$mode: the recorded edited copy $e was not reported: $(cat "$H/err")"
  done
  ! grep -Fq "$edited_cp" "$H/err" || fail "$mode: the unrecorded edited Copilot copy was reported"
  [[ -L "$H/.claude/agents/planar-local-helper.md" && "$(cat "$H/.claude/agents/planar-local-helper.md")" == helper ]] \
    || fail "$mode: the planar-local link was touched"
  [[ -f "$H/.codex/agents/planar-local-helper.toml" ]] || fail "$mode: the planar-local Codex agent was removed"
  [[ -L "$H/.gemini/antigravity-cli/agents/planar-local-pinned.md" ]] || fail "$mode: a planar-local link into the old agents/ tree was removed"
  [[ -f "$P/local/agents/helper.md" ]] || fail "$mode: \$PLANAR_HOME/local/ was touched"
  [[ "$(cat "$H/.gemini/agents/planar-mine.md")" == mine && -f "$H/.claude/agents/notes.md" ]] || fail "$mode: an operator file was removed"
  [[ -L "$H/.copilot/agents/planar-x.agent.md" ]] || fail "$mode: a planar-* link outside the staged trees was removed"
  ! grep -Fq 'planar-mine.md' "$H/err" || fail "$mode: the operator's own planar-mine.md was reported"
  for d in .claude/agents .codex/agents .copilot/agents .gemini/agents .gemini/antigravity-cli/agents .config/opencode/agents; do
    [[ -d "$H/$d" ]] || fail "$mode: the vendor directory $d was removed"
  done
  check_staged "$REPO" "$H"
  ! grep -Fq '"kind": "agent"' "$P/install-manifest.json" || fail "$mode: the new manifest still records an agent row"
  ! grep -Fq "$edited_md" "$P/install-manifest.json" || fail "$mode: the new manifest still records an edited agent"
  run_install "$REPO" "$H" ${flag[@]+"${flag[@]}"} || fail "$mode: re-run after the upgrade failed: $(cat "$H/err")"
  ! grep -Fq 'removed role agent' "$H/out" || fail "$mode: the re-run removed a role agent: $(grep removed "$H/out")"
done

fi
if scen flags; then
mark flags
# --- conflicting installer flags are refused by name (plan 1133, rel-m5-installer-dedup) -------
# Each refusal exits before anything is created: no prefix appears. --prebuilt
# with no directory keeps the usage exit 64.
H="$TMP/h32"; mkdir -p "$H"
run_install "$REPO" "$H" --prebuilt "$TMP/no-such-bundle" --ignore-live-queue && rc=0 || rc=$?
[[ "$rc" == 2 ]] || fail "--prebuilt with --ignore-live-queue exited $rc, not 2"
grep -Fq -- '--ignore-live-queue' "$H/err" || fail "the refusal does not name --ignore-live-queue: $(cat "$H/err")"
grep -Fq -- '--prebuilt' "$H/err" || fail "the refusal does not name both flags: $(cat "$H/err")"
[[ ! -e "$H/.planar" ]] || fail "--prebuilt with --ignore-live-queue created the prefix"
run_install "$REPO" "$H" --ignore-live-queue --prebuilt "$TMP/no-such-bundle" && rc=0 || rc=$?
[[ "$rc" == 2 ]] || fail "--ignore-live-queue before --prebuilt exited $rc, not 2"

run_install "$REPO" "$H" --uninstall --prebuilt "$TMP/no-such-bundle" && rc=0 || rc=$?
[[ "$rc" == 2 ]] || fail "--prebuilt with --uninstall exited $rc, not 2: $(cat "$H/err")"
grep -Fq -- '--uninstall' "$H/err" || fail "the refusal does not name --uninstall: $(cat "$H/err")"
grep -Fq -- '--prebuilt' "$H/err" || fail "the refusal does not name both flags: $(cat "$H/err")"
[[ ! -e "$H/.planar" ]] || fail "--prebuilt with --uninstall created the prefix"

for argv in "--prebuilt" "--prebuilt --force" "--prebuilt ''"; do
  eval "run_install \"\$REPO\" \"\$H\" $argv" && rc=0 || rc=$?
  [[ "$rc" == 64 ]] || fail "'install.sh $argv' exited $rc, not the usage exit 64"
  grep -Fq -- '--prebuilt needs a bundle directory' "$H/err" || fail "'install.sh $argv' did not name --prebuilt: $(cat "$H/err")"
done

fi
if scen lists; then
mark lists
# --- one copy of the managed-subtree and vendor lists ------------------------------------------
# install.sh, the uninstaller and the ownership rules source
# scripts/install-lib/managed-lists.sh; none keeps a copy of either list.
LISTS="$ROOT/scripts/install-lib/managed-lists.sh"
[[ -f "$LISTS" ]] || fail "scripts/install-lib/managed-lists.sh is missing"
subtrees="$(bash -c 'source "$1"; printf "%s" "$PLANAR_JOURNAL_SUBTREES"' x "$LISTS")"
vendors="$(bash -c 'source "$1"; printf "%s" "$PLANAR_VENDOR_NAMES"' x "$LISTS")"
[[ "$subtrees" == "bin skills scripts migrations" ]] || fail "unexpected managed subtrees: $subtrees"
[[ "$vendors" == "claude codex copilot gemini antigravity opencode" ]] || fail "unexpected vendor names: $vendors"
for f in install.sh scripts/uninstall.sh scripts/install-lib/ownership.sh; do
  grep -Fq 'managed-lists.sh' "$ROOT/$f" || fail "$f does not source managed-lists.sh"
  ! grep -Eq '^[[:space:]]*(PLANAR_JOURNAL_SUBTREES|PLANAR_VENDOR_NAMES)=' "$ROOT/$f" || fail "$f defines a list of its own"
  ! grep -Fq 'bin skills scripts migrations' "$ROOT/$f" || fail "$f spells out the managed-subtree list"
  ! grep -Eq 'claude,? ?codex,? ?copilot' "$ROOT/$f" || fail "$f spells out the vendor list"
done
# Behaviour, not just text: a vendor added to the one list is known to the installer.
MUT="$TMP/repo-lists"; make_repo "$MUT"
printf '%s\n' 'PLANAR_VENDOR_NAMES="$PLANAR_VENDOR_NAMES zzvendor"' >> "$MUT/scripts/install-lib/managed-lists.sh"
H="$TMP/h33"; mk_home h33 claude
run_install "$MUT" "$H" --vendors zzvendor || fail "install with the extended vendor list failed: $(cat "$H/err")"
! grep -Fq 'unknown vendor: zzvendor' "$H/err" || fail "install.sh keeps its own vendor list: it did not see the vendor added to managed-lists.sh"
fi

[[ "$MARKED" -gt 0 ]] || { scen_none_ran; fail "group $GROUP ran no scenario"; }
printf 'install-stage tests (group %s%s): %s scenarios run\n' "$GROUP" "${SCEN_FILTER:+, scenario $SCEN_FILTER}" "$MARKED"
