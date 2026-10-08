#!/usr/bin/env bash
# Managed subtrees, retired paths and the templates rule, end to end through
# install.sh (plan 1122, task rel-managed-subtrees; tech spec 677, "Managed
# subtree", "Order of an install" step 7 and "The install is staged, probed, then
# swapped"; test spec 679):
#   - an install of unknown age becomes clean: every managed subtree equals the
#     bundle's byte for byte, files the release no longer ships are gone, the
#     retired paths (commands/, copilot/, bin/scriptorium, lib/libmtkahypar.dylib,
#     ...) are removed through install-cleanup.txt, every data path is untouched,
#     and no .staging-*, *.old or journal is left;
#   - a managed subtree that is a symlink (an operator's, or a --link install's)
#     is replaced by a directory without following it: the target is
#     byte-identical afterwards; a cleanup entry that is a symlink removes the
#     link only;
#   - switching between --link and copy mode replaces the subtree's form in both
#     directions and leaves the checkout byte-identical;
#   - a file mode, and a path's type (directory to file, file to symlink), may
#     change between releases and the install applies it;
#   - an incomplete release (a managed subtree absent from the bundle or the
#     checkout) is refused before any write: it is never a retirement;
#   - templates are placed missing-only, --force included, and an edited template
#     or a formerly shipped, renamed one survives even when install-cleanup.txt
#     names it; no cleanup entry removes a data path;
#   - resuming an interrupted source install refuses the other copy/link mode
#     and a checkout with uncommitted changes (or one git cannot read), naming
#     the reason; the clean resume in the original mode completes.
# Scratch HOME everywhere; the bundles are fakes of tiny executables
# (scripts/fixtures/prebuilt-bundle.sh) and the source runs use a stub cmake.
# Runs under stock bash 3.2.
# shellcheck disable=SC2016,SC2012,SC2015,SC1091  # literal $ in fixtures; ls for messages; A && B || fail is intended
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'chmod -R u+rwx "$TMP" 2>/dev/null || true; rm -rf "$TMP"' EXIT
fail() { printf 'install-managed-test: %s\n' "$*" >&2; exit 1; }
PASSED=0
pass() { PASSED=$((PASSED + 1)); printf 'ok %s %s\n' "$PASSED" "$1"; }
# shellcheck source=fixtures/prebuilt-bundle.sh
source "$ROOT/scripts/fixtures/prebuilt-bundle.sh"

# --- the PATH of a prebuilt install: install.sh's base tier only ---------------------

BASEBIN="$TMP/basebin"
mkdir -p "$BASEBIN"
base_names="$(sed -n '/^BASE_DEPS=(/,/^)/p' "$ROOT/install.sh" | sed -n 's/^  "\([^|"]*\)|.*/\1/p')"
[[ -n "$base_names" ]] || fail "could not read BASE_DEPS from install.sh"
for n in $base_names; do
  found="$(command -v "$n" || true)"
  [[ -n "$found" ]] || fail "base tool $n is not on this host"
  ln -s "$found" "$BASEBIN/$n"
done

# run_prebuilt HOME BUNDLE [ENV=V...] -- [args]: sets RC; output in $TMP/out, $TMP/err.
run_prebuilt() {
  local home="$1" bundle="$2"; shift 2
  local envs=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
  [[ "${1-}" == "--" ]] && shift
  RC=0
  ( cd "$home" && /usr/bin/env -i HOME="$home" PATH="$BASEBIN" NO_COLOR=1 LC_ALL=C TMPDIR="$TMP" \
      ${envs[@]+"${envs[@]}"} \
      /bin/bash "$bundle/install.sh" --prebuilt "$bundle" --no-vendor "$@" >"$TMP/out" 2>"$TMP/err" ) || RC=$?
}
new_home() { mkdir -p "$TMP/homes/$1"; printf '%s' "$TMP/homes/$1"; }

# tree_id DIR [nomode] -- every path under DIR (symlinks not followed) with its
# type, mode (unless nomode), checksum or link target: two identical trees give
# identical output.
tree_id() {
  ( cd "$1" && find . | LC_ALL=C sort | while IFS= read -r f; do
      m="$(ls -ld "$f" | awk '{print $1}')"
      [[ "${2-}" != nomode ]] || m=-
      if [[ -L "$f" ]]; then printf 'L %s -> %s\n' "$f" "$(readlink "$f")"
      elif [[ -d "$f" ]]; then printf 'D %s %s\n' "$f" "$m"
      else printf 'F %s %s %s\n' "$f" "$m" "$(cksum < "$f")"; fi
    done )
}
# same_tree A B -- the two directories hold the same bytes and the same symlinks.
same_tree() { [[ "$(tree_id "$1" nomode)" == "$(tree_id "$2" nomode)" ]]; }
# bundle_tree BUNDLE NAME -- what the install of BUNDLE places as NAME: the
# bundle's own subtree, except bin/, which also holds the bundle's uninstall.sh
# as planar-uninstall.
bundle_tree() {
  local b="$1" n="$2" e
  if [[ "$n" != bin ]]; then printf '%s' "$b/$n"; return 0; fi
  e="$TMP/expected-bin/$(printf '%s' "$b" | cksum | tr -d ' ')/bin"
  if [[ ! -d "$e" ]]; then
    mkdir -p "$(dirname "$e")"
    cp -R "$b/bin" "$e"
    cp "$b/uninstall.sh" "$e/planar-uninstall"
  fi
  printf '%s' "$e"
}
no_evidence() { # no_evidence ROOT -- no staging, backup or journal is left
  [[ -z "$(ls -d "$1"/*.old "$1"/.staging-* "$1/.planar-journal" 2>/dev/null)" ]] || fail "recovery evidence is left: $(ls -a "$1")"
}
MANAGED="bin skills agents codex-agents workflows scripts migrations"

# --- 1. an install of unknown age becomes clean ----------------------------------------------

BUNDLE="$TMP/bundle/planar-fake"
fake_bundle_make "$ROOT" "$BUNDLE"
H="$(new_home age)"
P="$H/.planar"
mkdir -p "$P/bin" "$P/lib" "$P/opt/mtkahypar" "$P/commands" "$P/copilot/prompts" "$P/skills/codex" "$P/skills/planar" \
         "$P/agents/claude" "$P/codex-skills/x" "$P/workflows" "$P/scripts/old" "$P/codex-agents" "$P/migrations" \
         "$P/templates/doc-prompts" "$P/queue-logs" "$P/retired/2026-01-01" "$P/workbench/x" "$P/local" "$P/models" "$P/execute" "$P/workspaces"
for f in bin/planar bin/scriptorium bin/mtkahypar lib/libmtkahypar.dylib opt/mtkahypar/wheel commands/pl-old.md copilot/prompts/p.md \
         skills/codex/s.md skills/planar/OLD.md agents/claude/c.md codex-skills/x/s.md workflows/old.lua scripts/old/gone.sh \
         scripts/stray.sh codex-agents/planar-gone.toml migrations/00001_ancient.sql; do
  printf 'old %s\n' "$f" > "$P/$f"
done
chmod 755 "$P/bin/planar"
printf 'edited template\n' > "$P/templates/a.toml"
printf 'formerly shipped\n' > "$P/templates/doc-prompts/glossary.md"
printf 'log 7\n' > "$P/queue-logs/7.log"
printf 'legacy agent.db\n' > "$P/retired/2026-01-01/agent.db"
printf 'readme\n' > "$P/workbench/x/README.md"
printf '[cfg]\n' > "$P/config.toml"
printf 'SQLite format 3\n' > "$P/planar.db"
chmod 600 "$P/planar.db"
DATA="queue-logs retired workbench config.toml"
data_before="$(cd "$P" && for d in $DATA; do tree_id "$d" 2>/dev/null || cksum "$d"; done)"
run_prebuilt "$H" "$BUNDLE" --
[[ "$RC" == 0 ]] || fail "the install over an install of unknown age failed ($RC): $(cat "$TMP/err") $(cat "$TMP/out")"
for n in bin skills agents codex-agents workflows scripts migrations; do
  same_tree "$(bundle_tree "$BUNDLE" "$n")" "$P/$n" || fail "$n/ is not the bundle's: $(diff -r "$(bundle_tree "$BUNDLE" "$n")" "$P/$n" | head)"
done
for b in planar planar-agent planar-watch planar-execute planar-ext; do
  cmp -s "$BUNDLE/bin/$b" "$P/bin/$b" && [[ -x "$P/bin/$b" ]] || fail "bin/$b is not the bundle's executable"
done
cmp -s "$BUNDLE/uninstall.sh" "$P/bin/planar-uninstall" && [[ -x "$P/bin/planar-uninstall" ]] \
  || fail "bin/planar-uninstall is not the bundle's uninstall.sh, executable"
for gone in bin/scriptorium bin/mtkahypar lib/libmtkahypar.dylib opt/mtkahypar commands copilot skills/codex agents/claude codex-skills \
            scripts/stray.sh scripts/old workflows/old.lua codex-agents/planar-gone.toml migrations/00001_ancient.sql skills/planar/OLD.md; do
  [[ ! -e "$P/$gone" && ! -L "$P/$gone" ]] || fail "$gone survived the install"
done
no_evidence "$P"
[[ "$(cd "$P" && for d in $DATA; do tree_id "$d" 2>/dev/null || cksum "$d"; done)" == "$data_before" ]] || fail "the install changed a data path"
[[ "$(cat "$P/templates/a.toml")" == "edited template" && -f "$P/templates/b.toml" ]] || fail "templates were not kept and completed"
[[ "$(cat "$P/planar.db")" == "SQLite format 3" ]] || fail "planar.db changed"
grep -Eq 'removed stale dir +.*/commands/' "$TMP/out" && grep -Eq 'removed stale dir +.*/copilot/' "$TMP/out" \
  || fail "the retirement of commands/ and copilot/ was not reported: $(cat "$TMP/out")"
# A second run changes nothing.
snap="$(for n in $MANAGED; do tree_id "$P/$n"; done)"
run_prebuilt "$H" "$BUNDLE" --
[[ "$RC" == 0 && "$(for n in $MANAGED; do tree_id "$P/$n"; done)" == "$snap" ]] || fail "a second install changed the managed subtrees"
no_evidence "$P"
pass "an install of unknown age becomes clean: subtrees equal the bundle's, retired paths gone, data untouched, no evidence left"

# --- 2. symlinked managed subtrees and cleanup entries are never followed -----------------------

H="$(new_home links)"
P="$H/.planar"
OUT="$TMP/outside"
mkdir -p "$P" "$OUT/scripts-target" "$OUT/workflows-target" "$OUT/commands-target" "$OUT/copilot-target"
printf 'precious\n' > "$OUT/scripts-target/keep.sh"
printf 'precious\n' > "$OUT/workflows-target/keep.lua"
printf 'precious\n' > "$OUT/commands-target/keep.md"
printf 'precious\n' > "$OUT/copilot-target/keep.md"
mkdir -p "$P/bin"; printf 'old\n' > "$P/bin/planar"; chmod 755 "$P/bin/planar"
ln -s "$OUT/scripts-target" "$P/scripts"
ln -s "$OUT/workflows-target" "$P/workflows"
ln -s "$OUT/commands-target" "$P/commands"
ln -s "$OUT/copilot-target" "$P/copilot"
out_before="$(tree_id "$OUT")"
run_prebuilt "$H" "$BUNDLE" --
[[ "$RC" == 0 ]] || fail "install over symlinked subtrees failed ($RC): $(cat "$TMP/err") $(cat "$TMP/out")"
[[ "$(tree_id "$OUT")" == "$out_before" ]] || fail "a symlink target was changed: $(tree_id "$OUT" | diff - <(printf '%s\n' "$out_before"))"
for n in scripts workflows; do
  [[ -d "$P/$n" && ! -L "$P/$n" ]] || fail "$n/ is still a symlink after the install"
  same_tree "$BUNDLE/$n" "$P/$n" || fail "$n/ is not the bundle's after replacing a symlink"
done
[[ ! -e "$P/commands" && ! -L "$P/commands" && ! -e "$P/copilot" && ! -L "$P/copilot" ]] || fail "a retired symlink survived"
no_evidence "$P"
pass "a symlinked managed subtree is replaced by a directory and a symlinked retired path is unlinked, never followed"

# --- 3. a type or mode change between releases is applied ------------------------------------------

B1="$TMP/bundle/rel1"; B2="$TMP/bundle/rel2"
fake_bundle_make "$ROOT" "$B1"; fake_bundle_make "$ROOT" "$B2"
mkdir -p "$B1/workflows/sub" "$B1/migrations/x" "$B2/migrations"
printf 'a\n' > "$B1/workflows/run.sh";  chmod 644 "$B1/workflows/run.sh"
printf 'b\n' > "$B1/workflows/tool.sh"; chmod 755 "$B1/workflows/tool.sh"
printf 'c\n' > "$B1/workflows/sub/f"          # a directory in release 1 ...
printf 'x\n' > "$B1/workflows/link-me"        # ... a file that becomes a symlink in release 2
printf 'a\n' > "$B2/workflows/run.sh";  chmod 755 "$B2/workflows/run.sh"
printf 'b\n' > "$B2/workflows/tool.sh"; chmod 644 "$B2/workflows/tool.sh"
printf 'now a file\n' > "$B2/workflows/sub"  # ... and a file in release 2
printf 'target\n' > "$B2/workflows/real"
ln -s real "$B2/workflows/link-me"
printf 'y\n' > "$B1/migrations/x/m.sql"; printf 'file now\n' > "$B2/migrations/x"
H="$(new_home types)"; P="$H/.planar"
run_prebuilt "$H" "$B1" --
[[ "$RC" == 0 ]] || fail "release 1 failed ($RC): $(cat "$TMP/err")"
[[ -d "$P/workflows/sub" && ! -x "$P/workflows/run.sh" && -x "$P/workflows/tool.sh" && -f "$P/workflows/link-me" && ! -L "$P/workflows/link-me" ]] || fail "release 1 was not installed as built"
run_prebuilt "$H" "$B2" --
[[ "$RC" == 0 ]] || fail "release 2 failed ($RC): $(cat "$TMP/err") $(cat "$TMP/out")"
[[ -x "$P/workflows/run.sh" ]] || fail "an executable bit added by the release was not applied"
[[ ! -x "$P/workflows/tool.sh" ]] || fail "an executable bit removed by the release was kept"
[[ -f "$P/workflows/sub" && ! -d "$P/workflows/sub" ]] || fail "a directory that became a file was not replaced"
[[ -L "$P/workflows/link-me" && "$(readlink "$P/workflows/link-me")" == real ]] || fail "a file that became a symlink was not replaced"
[[ -f "$P/migrations/x" && ! -d "$P/migrations/x" ]] || fail "migrations/x did not change type"
same_tree "$B2/workflows" "$P/workflows" && same_tree "$B2/migrations" "$P/migrations" || fail "the subtrees are not release 2's"
no_evidence "$P"
pass "executable-bit and type changes between releases are applied (directory to file, file to symlink)"

# --- 4. an incomplete release is refused before anything is written ----------------------------------
# A managed subtree is always shipped. One that is absent from the source is a
# broken bundle or checkout, never a retirement: the installed tree must survive.

H="$(new_home incomplete)"; P="$H/.planar"
run_prebuilt "$H" "$B2" --
[[ "$RC" == 0 ]] || fail "release 2 failed ($RC): $(cat "$TMP/err")"
arena_before="$(tree_id "$P")"
for missing in workflows migrations scripts/install-lib scripts skills/planar codex-agents uninstall.sh; do
  BM="$TMP/bundle/missing-${missing//\//-}"
  fake_bundle_make "$ROOT" "$BM"
  rm -rf "${BM:?}/$missing"
  run_prebuilt "$H" "$BM" --
  [[ "$RC" != 0 ]] || fail "a bundle without $missing/ was installed"
  grep -Fq "$missing" "$TMP/err" || fail "the refusal does not name $missing: $(cat "$TMP/err")"
  ! grep -Fq "Staging into" "$TMP/out" || fail "a bundle without $missing/ was refused only after staging began: $(cat "$TMP/out")"
  [[ "$(tree_id "$P")" == "$arena_before" ]] || fail "a bundle without $missing/ changed the install: $(tree_id "$P" | diff - <(printf '%s\n' "$arena_before") | head)"
done
# On a fresh home nothing is created either.
H2="$(new_home incomplete-fresh)"
for missing in workflows migrations scripts/install-lib; do
  BM="$TMP/bundle/missing-fresh"
  rm -rf "$BM"
  fake_bundle_make "$ROOT" "$BM"
  rm -rf "${BM:?}/$missing"
  run_prebuilt "$H2" "$BM" --
  [[ "$RC" != 0 && ! -e "$H2/.planar" ]] || fail "a bundle without $missing/ created ~/.planar on a fresh home ($RC)"
done
# The bundle's own install.sh cannot run without its scripts/install-lib, so the
# case above never reaches the completeness check for it. The checkout's
# installer can: REPO_ROOT is the checkout, sourcing succeeds, and only the
# completeness check stands between a bundle lacking scripts/install-lib and a
# half-created ~/.planar.
H3="$(new_home incomplete-checkout-installer)"
BM="$TMP/bundle/missing-install-lib-checkout"
fake_bundle_make "$ROOT" "$BM"
rm -rf "${BM:?}/scripts/install-lib"
RC=0
( cd "$H3" && /usr/bin/env -i HOME="$H3" PATH="$BASEBIN" NO_COLOR=1 LC_ALL=C TMPDIR="$TMP" \
    /bin/bash "$ROOT/install.sh" --prebuilt "$BM" --no-vendor >"$TMP/out" 2>"$TMP/err" ) || RC=$?
[[ "$RC" != 0 ]] || fail "the checkout installer accepted a bundle without scripts/install-lib/"
grep -Fq "scripts/install-lib" "$TMP/err" || fail "the refusal does not name scripts/install-lib: $(cat "$TMP/err")"
! grep -Fq "Staging into" "$TMP/out" || fail "a bundle without scripts/install-lib/ was refused only after staging began"
[[ ! -e "$H3/.planar" && ! -e "$H3/.planar.lock" ]] || fail "a bundle without scripts/install-lib/ left state behind: $(ls -A "$H3")"
pass "a prebuilt bundle missing a managed subtree is refused, naming it, with the installed tree and a fresh home unchanged"

# The source install refuses the same way when the checkout lacks a source directory.
# (Its fixture checkout is built in section 6; that case is section 7.)

# The swap itself refuses a subtree with no staged copy and renames nothing.
SWAP="$TMP/swap"; mkdir -p "$SWAP/root/workflows" "$SWAP/stage"
printf 'live\n' > "$SWAP/root/workflows/w.lua"
swap_before="$(tree_id "$SWAP")"
swap_out="$( cd "$SWAP" && /bin/bash -c '
  set -u
  for l in journal mutation-lock prefix-guard data-paths install-state; do
    [ -f "$1/scripts/install-lib/$l.sh" ] && source "$1/scripts/install-lib/$l.sh"
  done
  PLANAR_JOURNAL_SUBTREES="workflows"
  planar_journal_clear
  J_phase=mutating; J_sub_workflows=pending
  if planar_state_swap "$2/root" "$2/stage" workflows; then echo SWAPPED; else echo "REFUSED: $INSTALL_STATE_ERROR"; fi
' _ "$ROOT" "$SWAP" 2>&1 )"
[[ "$swap_out" == REFUSED:*missing* ]] || fail "planar_state_swap did not refuse a missing staged copy: $swap_out"
[[ "$(tree_id "$SWAP" | grep -v journal)" == "$(printf '%s\n' "$swap_before" | grep -v journal)" ]] || fail "a refused swap renamed something: $(tree_id "$SWAP")"
[[ -f "$SWAP/root/workflows/w.lua" && ! -e "$SWAP/root/workflows.old" ]] || fail "a refused swap moved the live subtree"
pass "planar_state_swap refuses a subtree with no staged copy and renames nothing"

# --- 5. templates are missing-only, with or without --force --------------------------------------------

B4="$TMP/bundle/rel4"
{
  cat "$ROOT/install-cleanup.txt"
  printf 'templates/old-name.toml\ntemplates/doc-prompts/glossary.md\ntemplates/a.toml\ntemplates/\n'
  printf 'queue-logs/\nretired/\nworkbench/\nconfig.toml\nplanar.db\nplanar.db-wal\nlocal/\nmodels/\nworkspaces/\nexecute/\n'
  printf 'opt/genuinely-stale/\n'
} > "$TMP/cleanup4.txt"
fake_bundle_make "$ROOT" "$B4" "$TMP/cleanup4.txt"
printf 'shipped c\n' > "$B4/templates/c.toml"
H="$(new_home templates)"; P="$H/.planar"
mkdir -p "$P/templates/doc-prompts" "$P/opt/genuinely-stale" "$P/queue-logs" "$P/retired/old" "$P/workbench/w" "$P/local/l" \
         "$P/models" "$P/workspaces" "$P/execute"
printf 'my edit\n' > "$P/templates/a.toml"
printf 'renamed away\n' > "$P/templates/old-name.toml"
printf 'old prompt\n' > "$P/templates/doc-prompts/glossary.md"
ln -s "$TMP/outside/commands-target/keep.md" "$P/templates/linked.toml"
ln -s "$TMP/outside/commands-target/keep.md" "$P/templates/b.toml"
printf 'stale\n' > "$P/opt/genuinely-stale/f"
printf 'l\n' > "$P/queue-logs/1.log"; printf 'a\n' > "$P/retired/old/agent.db"; printf 'w\n' > "$P/workbench/w/x"
printf 'c\n' > "$P/config.toml"; printf 'SQLite format 3\n' > "$P/planar.db"; printf 'wal\n' > "$P/planar.db-wal"
printf 'l\n' > "$P/local/l/x"; printf 'm\n' > "$P/models/x"; printf 'w\n' > "$P/workspaces/x"; printf 'e\n' > "$P/execute/x"
DATA2="templates queue-logs retired workbench config.toml planar.db planar.db-wal local models workspaces execute"
snapshot() { ( cd "$P" && for d in $DATA2; do if [[ -d "$d" ]]; then tree_id "$d"; else cksum "$d"; fi; done ); }
before="$(snapshot)"
for force in "" "--force"; do
  run_prebuilt "$H" "$B4" -- $force
  [[ "$RC" == 0 ]] || fail "install ${force:-(ordinary)} failed ($RC): $(cat "$TMP/err") $(cat "$TMP/out")"
  [[ "$(cat "$P/templates/a.toml")" == "my edit" ]] || fail "${force:-ordinary}: an edited template was overwritten"
  [[ "$(cat "$P/templates/old-name.toml")" == "renamed away" && "$(cat "$P/templates/doc-prompts/glossary.md")" == "old prompt" ]] \
    || fail "${force:-ordinary}: a formerly shipped template was removed by a cleanup entry"
  [[ -L "$P/templates/b.toml" && "$(readlink "$P/templates/b.toml")" == "$TMP/outside/commands-target/keep.md" ]] \
    || fail "${force:-ordinary}: a symlinked template was replaced"
  [[ "$(cat "$TMP/outside/commands-target/keep.md")" == precious ]] || fail "a template symlink was written through"
  [[ "$(cat "$P/templates/c.toml")" == "shipped c" ]] || fail "${force:-ordinary}: a new default was not placed"
  [[ ! -e "$P/opt/genuinely-stale" ]] || fail "${force:-ordinary}: a genuine cleanup entry was not applied"
  no_evidence "$P"
  rm -f "$P/templates/c.toml"
  grep -Fq "data path" "$TMP/out" || fail "the skipped cleanup entries were not named: $(cat "$TMP/out")"
  mkdir -p "$P/opt/genuinely-stale"; printf 'stale\n' > "$P/opt/genuinely-stale/f"
  after="$(snapshot)"
  [[ "$after" == "$before" ]] || fail "${force:-ordinary}: a data path changed: $(printf '%s\n' "$after" | diff - <(printf '%s\n' "$before") | head)"
done
pass "templates stay missing-only under --force; no cleanup entry removes a data path or a template"

# --- 6. link and copy mode replace each other, the checkout is never touched ------------------------

REPO="$TMP/repo"
mkdir -p "$REPO/skills" "$REPO/templates" "$REPO/workflows" "$REPO/migrations"
cp "$ROOT/install.sh" "$REPO/install.sh"
cp -R "$ROOT/scripts" "$REPO/scripts"
rm -rf "$REPO/scripts/__pycache__" "$REPO/scripts/install-lib/__pycache__"
cp -R "$ROOT/agents" "$REPO/agents"
cp -R "$ROOT/skills/planar" "$REPO/skills/planar"
cp "$ROOT/install-cleanup.txt" "$REPO/install-cleanup.txt"
printf 'shipped a\n' > "$REPO/templates/a.toml"
printf -- '-- wf\n' > "$REPO/workflows/w.lua"
printf -- '-- m\n' > "$REPO/migrations/00001_x.up.sql"
: > "$REPO/CMakeLists.txt"
: > "$REPO/CMakePresets.json"
perl -0pi -e 's#/opt/homebrew/opt/llvm/bin/clang(\+\+)?#/bin/sh#g' "$REPO/install.sh"
STUBS="$TMP/stubs"
mkdir -p "$STUBS"
cat > "$STUBS/cmake" <<STUB
#!/usr/bin/env bash
if [[ "\$1" == "--install" ]]; then
  source "$ROOT/scripts/fixtures/prebuilt-bundle.sh"
  mkdir -p "\$4/bin"
  for b in planar planar-agent planar-watch planar-execute planar-ext; do
    stub_binary_write "\$4/bin/\$b" "\${STUB_TAG:-dev}"
  done
fi
exit 0
STUB
chmod +x "$STUBS/cmake"
run_source() { # run_source HOME [args]
  local home="$1"; shift
  RC=0
  env -u CODEX_HOME -u PLANAR_HOME -u PLANAR_DB -u PLANAR_CONFIG_PATH PATH="$STUBS:$PATH" HOME="$home" NO_COLOR=1 \
    "$REPO/install.sh" --build-dir "$home/build" --no-vendor --prefix "$home/.planar" "$@" >"$TMP/out" 2>"$TMP/err" || RC=$?
}
H="$(new_home mode)"; P="$H/.planar"
repo_before="$(tree_id "$REPO")"
run_source "$H" --link
[[ "$RC" == 0 ]] || fail "--link install failed ($RC): $(cat "$TMP/err")"
for n in scripts workflows migrations; do
  [[ -L "$P/$n" && "$(readlink "$P/$n")" == "$REPO/$n" ]] || fail "--link: $n/ is not a symlink into the checkout"
done
no_evidence "$P"
run_source "$H"
[[ "$RC" == 0 ]] || fail "the copy install over a link install failed ($RC): $(cat "$TMP/err")"
for n in scripts workflows migrations; do
  [[ -d "$P/$n" && ! -L "$P/$n" ]] || fail "copy mode: $n/ is still a link"
  same_tree "$REPO/$n" "$P/$n" || fail "copy mode: $n/ is not a copy of the checkout: $(diff -r "$REPO/$n" "$P/$n" | head)"
done
[[ "$(tree_id "$REPO")" == "$repo_before" ]] || fail "the checkout changed when link mode gave way to copy mode: $(tree_id "$REPO" | diff - <(printf '%s\n' "$repo_before") | head)"
no_evidence "$P"
run_source "$H" --link
[[ "$RC" == 0 ]] || fail "the link install over a copy install failed ($RC): $(cat "$TMP/err")"
for n in scripts workflows migrations; do
  [[ -L "$P/$n" && "$(readlink "$P/$n")" == "$REPO/$n" ]] || fail "--link again: $n/ is not a symlink into the checkout"
done
[[ "$(tree_id "$REPO")" == "$repo_before" ]] || fail "the checkout changed when copy mode gave way to link mode"
no_evidence "$P"
pass "switching between link and copy mode replaces the subtree's form and leaves the checkout byte-identical"

# --- 7. a checkout without a managed subtree's source is refused before any write -----------------------

REPO_GOOD="$REPO"
for missing in workflows migrations scripts/install-lib skills/planar agents; do
  REPO="$TMP/repo-missing-${missing//\//-}"
  cp -R "$REPO_GOOD" "$REPO"
  rm -rf "${REPO:?}/$missing"
  H="$(new_home "src-missing-${missing//\//-}")"
  run_source "$H"
  [[ "$RC" != 0 ]] || fail "a source install without $missing/ succeeded"
  grep -Fq "$missing" "$TMP/err" || fail "the refusal does not name $missing: $(cat "$TMP/err")"
  ! grep -Fq "Building" "$TMP/out" || fail "a checkout without $missing/ was refused only after the build began"
  [[ ! -e "$H/.planar" && ! -e "$H/build" ]] || fail "a source install without $missing/ wrote before refusing: $(ls -a "$H")"
done
REPO="$REPO_GOOD"
H="$(new_home src-missing-live)"; P="$H/.planar"
run_source "$H"
[[ "$RC" == 0 ]] || fail "the baseline source install failed ($RC): $(cat "$TMP/err")"
arena_before="$(tree_id "$P")"
REPO="$TMP/repo-missing-live"
cp -R "$REPO_GOOD" "$REPO"; rm -rf "$REPO/workflows"
run_source "$H"
[[ "$RC" != 0 && "$(tree_id "$P")" == "$arena_before" ]] || fail "a source install without workflows/ changed an installed tree ($RC)"
REPO="$REPO_GOOD"
pass "a source install whose checkout lacks a managed subtree's directory is refused, naming it, before any write"

# --uninstall needs no workflows/ or migrations/, so a checkout lacking them
# still uninstalls (the completeness check is for installs only).
REPO="$TMP/repo-uninstall-incomplete"
cp -R "$REPO_GOOD" "$REPO"; rm -rf "$REPO/workflows" "$REPO/migrations"
run_source "$H" --uninstall
[[ "$RC" == 0 ]] || fail "--uninstall from a checkout lacking workflows/ and migrations/ failed ($RC): $(cat "$TMP/err")"
[[ ! -e "$P/bin" && ! -e "$P/workflows" && ! -e "$P/install-manifest.json" ]] || fail "--uninstall from an incomplete checkout left managed trees: $(ls -A "$P" | tr '\n' ' ')"
REPO="$REPO_GOOD"
pass "--uninstall from a checkout lacking workflows/ and migrations/ still uninstalls"

# --- 8. a resume never mixes copy and link mode, or resumes from a dirty checkout ---------------------------
# Test spec 679, "Edge — a crashed owner's record survives a hostname change":
# a resume from a dirty checkout, or in the other copy/link mode, is refused
# naming the reason, and nothing changes; the clean resume in the original mode
# then completes. The checkout is a git repository, as an operator's is.

REPO="$TMP/repo-git"
cp -R "$REPO_GOOD" "$REPO"
git -C "$REPO" init -q
git -C "$REPO" add -A
git -C "$REPO" -c user.name=fixture -c user.email=fixture@example.invalid commit -qm fixture
# run_source_env HOME [ENV=V...] -- [args]: run_source with extra environment.
run_source_env() {
  local home="$1"; shift
  local envs=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
  [[ "${1-}" == "--" ]] && shift
  RC=0
  env -u CODEX_HOME -u PLANAR_HOME -u PLANAR_DB -u PLANAR_CONFIG_PATH PATH="$STUBS:$PATH" HOME="$home" NO_COLOR=1 \
    ${envs[@]+"${envs[@]}"} \
    "$REPO/install.sh" --build-dir "$home/build" --no-vendor --prefix "$home/.planar" "$@" >"$TMP/out" 2>"$TMP/err" || RC=$?
}
# kill_source NAME [args] -- a source install KILLed after its mutating record; sets H, P.
kill_source() {
  local name="$1"; shift
  H="$(new_home "$name")"; P="$H/.planar"
  run_source_env "$H" PLANAR_INSTALL_TEST_FAULT=kill@after-mutating PLANAR_INSTALL_TEST_FAULT_ARMED=test-only -- "$@"
  [[ "$RC" -ge 128 ]] || fail "$name: the source install was not killed ($RC): $(cat "$TMP/err")"
  grep -Fxq 'phase=mutating' "$P/.planar-journal" || fail "$name: no mutating journal: $(cat "$P/.planar-journal" 2>&1)"
}
# refused_resume WHAT PHRASE [args] -- the resume is refused at exit 1 naming PHRASE, changing nothing.
refused_resume() {
  local what="$1" phrase="$2" before; shift 2
  before="$(tree_id "$P")"
  run_source "$H" "$@"
  [[ "$RC" == 1 ]] || fail "$what was not refused ($RC): $(cat "$TMP/out" "$TMP/err")"
  grep -Fq "$phrase" "$TMP/err" || fail "$what: the refusal does not say '$phrase': $(cat "$TMP/err")"
  ! grep -Fq 'resuming the interrupted install' "$TMP/out" || fail "$what: the interrupted install was resumed"
  [[ "$(tree_id "$P")" == "$before" ]] || fail "$what: the refused run changed the installation"
  grep -Fxq 'phase=mutating' "$P/.planar-journal" || fail "$what: the refused run did not keep the mutating journal"
}

kill_source resume-copy
refused_resume "a link-mode resume of an interrupted copy-mode install" "was interrupted in copy mode and this run is in link mode" --link
printf 'edited\n' >> "$REPO/templates/a.toml"
refused_resume "a resume from a checkout with an uncommitted edit" "has uncommitted changes"
grep -Fq "$REPO" "$TMP/err" || fail "the dirty-checkout refusal does not name the checkout: $(cat "$TMP/err")"
git -C "$REPO" checkout -q -- templates/a.toml
printf -- '-- untracked\n' > "$REPO/workflows/untracked.lua"
refused_resume "a resume from a checkout with an untracked file" "has uncommitted changes"
rm -f "$REPO/workflows/untracked.lua"
run_source "$H"
[[ "$RC" == 0 ]] || fail "the clean resume in the original mode failed ($RC): $(cat "$TMP/err")"
grep -Fq 'resuming the interrupted install' "$TMP/out" || fail "the clean resume did not resume: $(cat "$TMP/out")"
for n in scripts workflows migrations; do
  [[ -d "$P/$n" && ! -L "$P/$n" ]] || fail "the copy-mode resume left $n/ a link"
done
no_evidence "$P"

kill_source resume-link --link
refused_resume "a copy-mode resume of an interrupted link-mode install" "was interrupted in link mode and this run is in copy mode"
run_source "$H" --link
[[ "$RC" == 0 ]] || fail "the clean link-mode resume failed ($RC): $(cat "$TMP/err")"
grep -Fq 'resuming the interrupted install' "$TMP/out" || fail "the clean link-mode resume did not resume: $(cat "$TMP/out")"
no_evidence "$P"

# A checkout git cannot read (no repository) cannot be shown clean: refused too.
REPO="$REPO_GOOD"
kill_source resume-nogit
refused_resume "a resume from a checkout that is not a git repository" "cannot tell whether the source checkout"
REPO="$REPO_GOOD"
pass "a resume refuses the other copy/link mode and a dirty or unreadable checkout, naming the reason; the clean resume completes"

printf 'install managed tests: %s passed\n' "$PASSED"
