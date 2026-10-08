#!/usr/bin/env bash
# Stock-macOS-bash safety of the installer (plan 1122, task rel-bash32; tech spec
# 677, "Prebuilt install mode", last paragraph). install.sh and every script it
# sources must run under /bin/bash 3.2 with `set -u`, which stock macOS ships.
#
#   static  every `${name[@]}` / `${name[*]}` in install.sh, scripts/install-lib/*.sh
#           and scripts/uninstall.sh (when present) is written in the guarded form
#           ${name[@]+"${name[@]}"} (a bash 3.2 `set -u` error on an empty array
#           otherwise), or carries the marker `# bash32: nonempty` on its line for an
#           array that is provably never empty; and none of those files uses a
#           bash-4-only construct (associative arrays, case-changing expansions,
#           mapfile, namerefs, &>>, |&, ;&, [[ -v ]], $EPOCHSECONDS, ${x@Q}).
#   dynamic a prebuilt install and an uninstall run under /bin/bash, with no vendor
#           present and with --no-vendor (the empty-array paths), then with vendors
#           present. On macOS /bin/bash must be 3.2; elsewhere the dynamic half
#           still runs under whatever /bin/bash is and only that version assertion
#           is skipped, with a printed reason. The end state is checked explicitly
#           and compared tree for tree (file and directory modes, symlink targets and
#           file checksums; run-specific lock records left out, the HOME path
#           normalised) between two runs under the old bash (a self-check that keeps
#           the comparison honest) and, when a different, newer bash is available,
#           with that bash's run. After every uninstall the mutation lock library's own
#           judgement (_pl_owner_state) must say no owner is live (free or released).
#
# THE TWO-SHELL COMPARISON AND WHERE IT IS REQUIRED (plan 1122 M5, task 7361)
#   The Linux gate image (docker/linux-gate.Dockerfile) builds a real bash 3.2.57 at
#   /opt/bash-3.2/bin/bash and sets PLANAR_BASH32_OLD to it and
#   PLANAR_BASH32_REQUIRE_COMPARE=1: there the old shell must be bash 3.x and a newer
#   bash (the image's /bin/bash) must exist, or the test FAILS rather than comparing
#   nothing. Anywhere else (a developer macOS host, whose /bin/bash is 3.2 but which
#   may have no newer bash) the comparison runs when a newer bash is found and
#   otherwise prints a note saying it did not run and why; it never silently passes
#   as if it had. PLANAR_BASH32_NEW names the newer bash explicitly.
#
# The uninstall command is a function (uninstall_run), so the standalone
# planar-uninstall can be driven by the same scenarios. Everything runs in a
# scratch HOME against a fake bundle; nothing real is read or built.
# shellcheck disable=SC2016,SC2012,SC1091  # literal $ in the lint patterns
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap '[[ -n "${KEEP:-}" ]] || rm -rf "$TMP"' EXIT
fail() { printf 'install-bash32-test: FAIL: %s\n' "$*" >&2; exit 1; }
PASSED=0
pass() { PASSED=$((PASSED + 1)); printf 'ok - %s\n' "$*"; }
# shellcheck source=fixtures/prebuilt-bundle.sh
source "$ROOT/scripts/fixtures/prebuilt-bundle.sh"

# ---- static half ---------------------------------------------------------------------

LINT_ROOT="${PLANAR_BASH32_LINT_ROOT:-$ROOT}"
lint_files() {
  local f
  for f in "$LINT_ROOT/install.sh" "$LINT_ROOT"/scripts/install-lib/*.sh "$LINT_ROOT/scripts/uninstall.sh"; do
    [[ -f "$f" ]] && printf '%s\n' "$f"
  done
}

# Flags: --static-only, --dynamic-only (the mutant probes isolate one half).
# lint_unguarded FILE -- print N:line for each unguarded array expansion, and for each
# braced "${@}" / "${*}" (an unbound-variable error under bash 3.2 `set -u` when there are
# no arguments; the plain "$@" is fine). The guarded form must repeat the same name:
# ${a[@]+"${a[@]}"}; a mismatched ${a[@]+"${b[@]}"} is flagged. The name match is done
# in bash because sed -E backreferences are not portable to BSD sed.
lint_unguarded() {
  local re='\$\{([A-Za-z_][A-Za-z0-9_]*)\[@\]\+"\$\{([A-Za-z_][A-Za-z0-9_]*)\[@\]\}"\}'
  local bare='\$\{[A-Za-z_][A-Za-z0-9_]*\[[@*]\]\}|\$\{[@*]\}'
  local n=0 line rest mismatch
  while IFS= read -r line || [[ -n "$line" ]]; do
    n=$((n + 1))
    [[ "$line" =~ ^[[:space:]]*# ]] && continue
    [[ "$line" == *'bash32: nonempty'* ]] && continue
    rest="$line"; mismatch=0
    while [[ "$rest" =~ $re ]]; do
      if [[ "${BASH_REMATCH[1]}" == "${BASH_REMATCH[2]}" ]]; then
        rest="${rest/"${BASH_REMATCH[0]}"/}"
      else
        mismatch=1; break
      fi
    done
    if [[ "$mismatch" == 1 ]] || [[ "$rest" =~ $bare ]]; then printf '%d:%s\n' "$n" "$line"; fi
  done < "$1"
}

# lint_bash4 FILE -- print N:line for each bash-4-only construct.
lint_bash4() {
  grep -n '' "$1" \
    | grep -Ev '^[0-9]+:[[:space:]]*#' \
    | grep -E 'declare -A|local -A|typeset -A|declare -n|local -n|mapfile|readarray|&>>|\|&|;&|;;&|\[\[ +-v |EPOCHSECONDS|EPOCHREALTIME|\$\{[A-Za-z_][A-Za-z0-9_]*(,,|\^\^|@Q|@E|@P|@A|@a)' || true
}

static_check() {
  local f hits bad=0
  [[ -n "$(lint_files)" ]] || fail "static: no files to lint"
  while IFS= read -r f; do
    hits="$(lint_unguarded "$f")"
    if [[ -n "$hits" ]]; then
      printf 'static: unguarded array expansion in %s (use ${a[@]+"${a[@]}"} or mark # bash32: nonempty):\n%s\n' "$f" "$hits" >&2
      bad=1
    fi
    hits="$(lint_bash4 "$f")"
    if [[ -n "$hits" ]]; then
      printf 'static: bash-4-only construct in %s:\n%s\n' "$f" "$hits" >&2
      bad=1
    fi
  done < <(lint_files)
  [[ "$bad" == 0 ]] || fail "static: bash 3.2 lint failed"
}

# The lint must itself detect what it claims to: a planted unguarded expansion, a
# planted bash-4 construct, and the marker and guarded forms must pass.
lint_selftest() {
  local f="$TMP/lint-self.sh"
  printf 'x=("$@")\nfor a in "${x[@]}"; do :; done\n' > "$f"
  [[ -n "$(lint_unguarded "$f")" ]] || fail "lint self-test: an unguarded \"\${x[@]}\" went undetected"
  printf 'echo "${x[*]}"\n' > "$f"
  [[ -n "$(lint_unguarded "$f")" ]] || fail "lint self-test: an unguarded \"\${x[*]}\" went undetected"
  printf 'for a in ${x[@]+"${x[@]}"}; do :; done\necho "${#x[@]} ${x[*]:-}"\nfor a in "${x[@]}"; do :; done # bash32: nonempty\n# "${x[@]}" in a comment\n' > "$f"
  [[ -z "$(lint_unguarded "$f")" ]] || fail "lint self-test: a guarded, marked or commented form was flagged: $(lint_unguarded "$f")"
  printf 'for a in ${x[@]+"${y[@]}"}; do :; done\n' > "$f"
  [[ -n "$(lint_unguarded "$f")" ]] || fail "lint self-test: a mismatched guard \${x[@]+\"\${y[@]}\"} went undetected"
  printf 'f "${@}"\n' > "$f"
  [[ -n "$(lint_unguarded "$f")" ]] || fail "lint self-test: a braced \"\${@}\" went undetected"
  printf 'f "${*}"\n' > "$f"
  [[ -n "$(lint_unguarded "$f")" ]] || fail "lint self-test: a braced \"\${*}\" went undetected"
  printf 'f "$@" "$*" "${#x[@]}" "${1-}" "${@:2}"\n' > "$f"
  [[ -z "$(lint_unguarded "$f")" ]] || fail "lint self-test: a plain \"\$@\" was flagged: $(lint_unguarded "$f")"
  printf 'declare -A m\n' > "$f"
  [[ -n "$(lint_bash4 "$f")" ]] || fail "lint self-test: declare -A went undetected"
  printf 'echo "${v,,}"\n' > "$f"
  [[ -n "$(lint_bash4 "$f")" ]] || fail "lint self-test: \${v,,} went undetected"
}

# Test groups (plan 1122 M5, task 7361): the whole file took about 250 s on a loaded host with
# the second shell, so ctest registers one entry per group, install.bash32 (static),
# install.bash32_flag, _home and _vendors (label install_bash32[_<group>]), each of
# which runs one scenario under the old bash, again, and under the newer bash.
# INSTALL_BASH32_GROUP selects one; unset runs everything in one process. An unknown
# name is a usage error.
GROUP="${INSTALL_BASH32_GROUP:-all}"
case "$GROUP" in all|static|flag|home|vendors) ;; *) printf 'install-bash32-test: unknown INSTALL_BASH32_GROUP %s (want one of: static flag home vendors)\n' "$GROUP" >&2; exit 2 ;; esac
case "$GROUP" in
  all)     SCEN_NAMES="novendor-flag novendor-home vendors" ;;
  flag)    SCEN_NAMES="novendor-flag" ;;
  home)    SCEN_NAMES="novendor-home" ;;
  vendors) SCEN_NAMES="vendors" ;;
  *)       SCEN_NAMES="" ;;
esac

if [[ "${1-}" != "--dynamic-only" && ( "$GROUP" == all || "$GROUP" == static ) ]]; then
  lint_selftest
  pass "static lint self-test: planted unguarded and bash-4 constructs are detected"
  static_check
  pass "static lint: no unguarded array expansion or bash-4-only construct in install.sh and its sourced scripts"
fi
[[ "$GROUP" == static ]] && { printf 'install-bash32-test: %d checks passed (static group)\n' "$PASSED"; exit 0; }
[[ "${1-}" == "--static-only" ]] && { printf 'install-bash32-test: %d checks passed (static only)\n' "$PASSED"; exit 0; }

# ---- dynamic half ---------------------------------------------------------------------

BASH_BIN="${PLANAR_BASH32_OLD:-/bin/bash}"
REQUIRE_COMPARE="${PLANAR_BASH32_REQUIRE_COMPARE:-}"
[[ -x "$BASH_BIN" ]] || fail "dynamic: $BASH_BIN is not executable"
child_major="$("$BASH_BIN" -c 'echo "${BASH_VERSINFO[0]}"')"
if [[ -n "$REQUIRE_COMPARE" ]]; then
  # The gate host: the old shell must really be bash 3.x, or the "newer" shell is the only shell.
  [[ "$child_major" == 3 ]] || fail "dynamic: PLANAR_BASH32_REQUIRE_COMPARE is set but $BASH_BIN is bash $child_major, not 3.x (set PLANAR_BASH32_OLD to a bash 3.2)"
  pass "dynamic: the old shell $BASH_BIN is bash 3.x (BASH_VERSINFO[0]=3)"
elif [[ "$(uname -s)" == Darwin && "$BASH_BIN" == /bin/bash ]]; then
  [[ "$child_major" == 3 ]] || fail "dynamic: /bin/bash on macOS must be bash 3.2 (BASH_VERSINFO[0]=$child_major); stock macOS ships 3.2"
  "$BASH_BIN" --version | head -n 1 | grep -q 'version 3\.2' || fail "dynamic: $BASH_BIN --version is not 3.2"
  pass "dynamic: /bin/bash is bash 3.2 (BASH_VERSINFO[0]=3)"
else
  printf 'note: skipping the "/bin/bash is 3.2" assertion: %s ships /bin/bash %s, not 3.2; the install and uninstall still run under it\n' "$(uname -s)" "$child_major"
fi

# A newer bash for the end-state comparison, when the host has one.
NEW_BASH=""
for c in "${PLANAR_BASH32_NEW:-}" /opt/homebrew/bin/bash /usr/local/bin/bash /bin/bash "$(command -v bash || true)"; do
  [[ -n "$c" && -x "$c" ]] || continue
  # the same file as the old shell (Debian: /bin -> /usr/bin) is no comparison at all
  [[ "$c" -ef "$BASH_BIN" ]] && continue
  m="$("$c" -c 'echo "${BASH_VERSINFO[0]}"' 2>/dev/null || true)"
  if [[ "$m" =~ ^[0-9]+$ && "$m" -gt "$child_major" && "$m" -ge 4 ]]; then NEW_BASH="$c"; break; fi
done
if [[ -n "$NEW_BASH" ]]; then
  printf 'note: comparing end states against %s (bash %s)\n' "$NEW_BASH" "$("$NEW_BASH" -c 'echo "${BASH_VERSINFO[0]}"')"
elif [[ -n "$REQUIRE_COMPARE" ]]; then
  fail "dynamic: PLANAR_BASH32_REQUIRE_COMPARE is set but no bash 4 or newer other than $BASH_BIN was found (PLANAR_BASH32_NEW=${PLANAR_BASH32_NEW:-unset}); the two-shell comparison cannot run"
else
  printf 'note: TWO-SHELL COMPARISON NOT RUN: no bash 4 or newer other than %s on this host and PLANAR_BASH32_REQUIRE_COMPARE is unset; the end state is checked against explicit expectations and a second run of the same shell only\n' "$BASH_BIN"
fi

# PATH: the base tier of install.sh's BASE_DEPS only, as install-prebuilt-test.sh does.
BASEBIN="$TMP/basebin"
mkdir -p "$BASEBIN"
base_names="$(sed -n '/^BASE_DEPS=(/,/^)/p' "$ROOT/install.sh" | sed -n 's/^  "\([^|"]*\)|.*/\1/p')"
[[ -n "$base_names" ]] || fail "could not read BASE_DEPS from install.sh"
for n in $base_names; do
  found="$(command -v "$n" || true)"
  [[ -n "$found" ]] || fail "base tool $n is not on this host"
  ln -s "$found" "$BASEBIN/$n"
done

BUNDLE="$TMP/bundle/planar-fake"
fake_bundle_make "$ROOT" "$BUNDLE"

# install_run BASH HOME [ARGS...] and uninstall_run BASH HOME: the commands under
# test, run from a scratch HOME with a scrubbed environment. Output in $TMP/out, $TMP/err.
run_in() {
  local bash_bin="$1" home="$2"; shift 2
  RC=0
  ( cd "$home" && /usr/bin/env -i HOME="$home" PATH="$BASEBIN" NO_COLOR=1 LC_ALL=C TMPDIR="$TMP" \
      "$bash_bin" "$@" >"$TMP/out" 2>"$TMP/err" ) || RC=$?
}
install_run() { local b="$1" h="$2"; shift 2; run_in "$b" "$h" "$BUNDLE/install.sh" --prebuilt "$BUNDLE" "$@"; }
UNINSTALL_REMOVES_VENDORS=1
# The installed standalone uninstaller (task 7314), run under the bash under test.
uninstall_run() { local b="$1" h="$2"; run_in "$b" "$h" "$h/.planar/bin/planar-uninstall"; }

# file_mode PATH -- the permission bits, octal. BSD stat (macOS) takes -f %Lp and GNU stat
# (Linux) -c %a; the form is chosen once by trying GNU's on /.
if stat -c %a / >/dev/null 2>&1; then file_mode() { stat -c %a "$1"; }; else file_mode() { stat -f %Lp "$1"; }; fi

# tree_state HOME -- every path under HOME with what makes it that state, for comparing runs:
# a regular file's mode and checksum, a directory's mode, a symlink's target. Run-specific
# state is normalised: .planar.lock/ (owner and released records) is left out, and the
# HOME path is replaced by @HOME@ inside every file and every link target before it is
# compared (the manifest and release stamp record absolute paths).
tree_state() { ( cd "$1" && find . -path ./.planar.lock -prune -o -print | LC_ALL=C sort | while IFS= read -r f; do
  if [[ -L "$f" ]]; then printf '%s -> %s\n' "$f" "$(readlink "$f" | LC_ALL=C sed "s#$1#@HOME@#g")"
  elif [[ -d "$f" ]]; then printf '%s dir %s\n' "$f" "$(file_mode "$f")"
  else printf '%s %s %s\n' "$f" "$(file_mode "$f")" "$(LC_ALL=C sed "s#$1#@HOME@#g" "$f" | cksum)"; fi
done ); }

# The state function must see what it claims to: a planted mode change, a planted symlink
# retarget, a changed directory mode and a moved file are each a difference.
tree_state_selftest() {
  local d="$TMP/ts-self" a b
  mkdir -p "$d/t/sub"; printf 'x\n' > "$d/t/f"; chmod 644 "$d/t/f"; ln -s one "$d/t/l"; chmod 755 "$d/t/sub"
  a="$(tree_state "$d/t")"
  [[ "$a" == "$(tree_state "$d/t")" ]] || fail "tree_state self-test: an unchanged tree compared different"
  chmod 600 "$d/t/f"; b="$(tree_state "$d/t")"
  [[ "$a" != "$b" ]] || fail "tree_state self-test: a planted file mode change (644 to 600) went undetected"
  chmod 644 "$d/t/f"; ln -sfn two "$d/t/l"; b="$(tree_state "$d/t")"
  [[ "$a" != "$b" ]] || fail "tree_state self-test: a planted symlink target change went undetected"
  ln -sfn one "$d/t/l"; chmod 700 "$d/t/sub"; b="$(tree_state "$d/t")"
  [[ "$a" != "$b" ]] || fail "tree_state self-test: a planted directory mode change went undetected"
  chmod 755 "$d/t/sub"; [[ "$a" == "$(tree_state "$d/t")" ]] || fail "tree_state self-test: restoring the tree did not restore its state"
}

# lock_state BASH HOME -- the mutation lock library's own judgement of the current owner of
# HOME/.planar (free, released, held, dead, ambiguous), or absent when no lock directory
# exists. It is computed under the bash being tested, with the base-tier PATH.
LOCKLIB="$ROOT/scripts/install-lib/mutation-lock.sh"
lock_state() {
  /usr/bin/env -i HOME="$2" PATH="$BASEBIN" LC_ALL=C "$1" -c '
    set -u; source "$1"; l="$(planar_lock_dir "$2")"
    [ -d "$l" ] || { echo absent; exit 0; }
    g="$(_pl_max_gen "$l")"; _pl_owner_state "$l" "$g"; echo "$_PL_STATE"' lock_state "$LOCKLIB" "$2/.planar"
}
# lock_clear STATE -- an uninstall leaves no owner: nothing ever locked, or the last
# generation released. A held, dead (killed) or ambiguous owner is a leftover.
lock_clear() { case "$1" in absent|free|released) return 0 ;; *) return 1 ;; esac; }

# The assertion must see a live owner: a real holder process acquires the lock of a scratch
# root; the library calls it held, and the assertion refuses it. Once it is killed, the
# record is dead, which the assertion also refuses (only a release counts).
lock_selftest() {
  local root="$TMP/lock-self" holder
  mkdir -p "$root"
  lock_clear "$(lock_state "$BASH_BIN" "$root")" || fail "lock self-test: a root with no lock directory was not clear"
  ( /usr/bin/env -i HOME="$root" PATH="$BASEBIN" LC_ALL=C "$BASH_BIN" -c 'source "$1"; planar_lock_acquire "$2" install || exit 1; : > "$3"; sleep 120' lk "$LOCKLIB" "$root/.planar" "$root/ready" >/dev/null 2>&1 ) &
  holder=$!
  local _; for _ in $(seq 1 100); do [[ -f "$root/ready" ]] && break; sleep 0.1; done
  [[ -f "$root/ready" ]] || { kill "$holder" 2>/dev/null; fail "lock self-test: the holder never acquired the lock"; }
  [[ "$(lock_state "$BASH_BIN" "$root")" == held ]] || { pkill -P "$holder" 2>/dev/null; kill "$holder" 2>/dev/null; fail "lock self-test: a live holder was judged $(lock_state "$BASH_BIN" "$root"), not held"; }
  ! lock_clear held || fail "lock self-test: lock_clear accepted a held owner"
  pkill -P "$holder" 2>/dev/null || true; kill "$holder" 2>/dev/null || true; wait "$holder" 2>/dev/null || true
  ! lock_clear "$(lock_state "$BASH_BIN" "$root")" || fail "lock self-test: lock_clear accepted a killed owner's record"
}

# scenario BASH TAG NAME VENDORS(0 none present, 1 present and placed, 2 present, --no-vendor) [install args] -- install, re-install, uninstall; echoes the
# post-install tree state into $TMP/state-<bash-tag>-<name>.{installed,removed}.
scenario() {
  local bash_bin="$1" tag="$2" name="$3" vendors="$4"; shift 4
  local home="$TMP/homes/$tag-$name" p
  mkdir -p "$home"
  [[ "$vendors" == 0 ]] || mkdir -p "$home/.claude" "$home/.codex"
  p="$home/.planar"
  install_run "$bash_bin" "$home" ${@+"$@"}
  [[ "$RC" == 0 ]] || fail "[$tag/$name] prebuilt install exited $RC: $(cat "$TMP/err") $(cat "$TMP/out")"
  grep -Eqi 'unbound variable|syntax error|bad substitution|command not found' "$TMP/err" "$TMP/out" && fail "[$tag/$name] install printed a shell error: $(cat "$TMP/err")"
  local b
  for b in planar planar-agent planar-watch planar-execute planar-ext; do
    cmp -s "$BUNDLE/bin/$b" "$p/bin/$b" || fail "[$tag/$name] bin/$b is not the bundle's"
  done
  [[ -f "$p/install-manifest.json" && -f "$p/release.json" && -f "$p/skills/planar/SKILL.md" ]] || fail "[$tag/$name] the install left no manifest, release.json or skill"
  if [[ "$vendors" == 1 ]]; then
    [[ -f "$home/.claude/agents/planar-coder.md" && -f "$home/.codex/agents/planar-coder.toml" ]] || fail "[$tag/$name] vendor surfaces were not placed"
  elif [[ "$vendors" == 2 ]]; then
    [[ ! -e "$home/.claude/agents/planar-coder.md" && ! -e "$home/.codex/agents/planar-coder.toml" ]] || fail "[$tag/$name] --no-vendor placed vendor surfaces"
  else
    [[ ! -e "$home/.claude" && ! -e "$home/.codex" ]] || fail "[$tag/$name] a vendor directory appeared in a vendorless HOME"
  fi
  # Two entries outside the managed tree that every run plants identically, so the state
  # comparison has a mode and a symlink target to compare. PLANT=mode or PLANT=link changes
  # one of them (the planted-change check below).
  printf 'planted\n' > "$home/planted-file"; chmod 644 "$home/planted-file"; ln -s target-a "$home/planted-link"
  [[ "${PLANT-}" != mode ]] || chmod 600 "$home/planted-file"
  [[ "${PLANT-}" != link ]] || ln -sfn target-b "$home/planted-link"
  tree_state "$home" > "$TMP/state-$tag-$name.installed"
  [[ "${STOP_AFTER-}" != installed ]] || return 0
  # a second install is the no-change path (more empty arrays: nothing to place)
  install_run "$bash_bin" "$home" ${@+"$@"}
  [[ "$RC" == 0 ]] || fail "[$tag/$name] second prebuilt install exited $RC: $(cat "$TMP/err")"
  uninstall_run "$bash_bin" "$home"
  [[ "$RC" == 0 ]] || fail "[$tag/$name] uninstall exited $RC: $(cat "$TMP/err") $(cat "$TMP/out")"
  grep -Eqi 'unbound variable|syntax error|bad substitution|command not found' "$TMP/err" "$TMP/out" && fail "[$tag/$name] uninstall printed a shell error: $(cat "$TMP/err")"
  [[ ! -e "$p/bin" && ! -e "$p/install-manifest.json" && ! -e "$p/skills" ]] || fail "[$tag/$name] uninstall left managed trees: $(ls -A "$p" | tr '\n' ' ')"
  # planar-uninstall reads the manifest with sed (no python3 is on this PATH) and
  # removes the vendor surfaces it recorded.
  if [[ "$vendors" == 1 && "$UNINSTALL_REMOVES_VENDORS" == 1 ]]; then
    [[ ! -e "$home/.claude/agents/planar-coder.md" && ! -e "$home/.codex/agents/planar-coder.toml" ]] || fail "[$tag/$name] uninstall left vendor surfaces"
  fi
  local ls; ls="$(lock_state "$bash_bin" "$home")"
  lock_clear "$ls" || fail "[$tag/$name] after uninstall the mutation lock library judges the owner $ls (a live or unreleased owner remains)"
  tree_state "$home" > "$TMP/state-$tag-$name.removed"
}

tree_state_selftest
pass "dynamic: tree_state self-test: planted mode, symlink-target and directory-mode changes are detected"
lock_selftest
pass "dynamic: lock assertion self-test: a live owner is held and refused; a killed owner is refused"
# scen_run BASH TAG NAME -- one named scenario (the three differ in vendors present and flags).
scen_run() {
  case "$3" in
    novendor-flag) scenario "$1" "$2" novendor-flag 2 --no-vendor ;;
    novendor-home) scenario "$1" "$2" novendor-home 0 ;;
    vendors)       scenario "$1" "$2" vendors 1 ;;
    *) fail "unknown scenario $3" ;;
  esac
}
# run_scenarios BASH TAG -- the selected group's scenarios under BASH.
run_scenarios() { local n; for n in $SCEN_NAMES; do scen_run "$1" "$2" "$n"; done; }
run_scenarios "$BASH_BIN" stock
pass "dynamic: prebuilt install and uninstall exit 0 under $BASH_BIN ($SCEN_NAMES), the end state as expected, no live lock owner left"

# compare_runs TAG_A TAG_B LABEL -- the three scenarios' end states under two tags must be
# identical once the scenario tag in the HOME path is normalised.
compare_runs() {
  local a="$1" b="$2" label="$3" s ph
  for s in $SCEN_NAMES; do
    for ph in installed removed; do
      diff "$TMP/state-$a-$s.$ph" "$TMP/state-$b-$s.$ph" > "$TMP/diff.out" \
        || fail "[$s/$ph] end state differs between $label: $(cat "$TMP/diff.out")"
    done
  done
}

# Self-check: the same bash run twice must compare equal, whether or not the host has a
# newer bash, so the comparison itself cannot rot unnoticed.
run_scenarios "$BASH_BIN" again
compare_runs stock again "two runs under $BASH_BIN"
pass "dynamic: comparison self-check: two runs under $BASH_BIN have identical end states"

if [[ -n "$NEW_BASH" ]]; then
  run_scenarios "$NEW_BASH" new
  compare_runs stock new "$BASH_BIN and $NEW_BASH"
  pass "dynamic: end states (modes, symlink targets, checksums) under $BASH_BIN and $NEW_BASH are identical"
  # A planted difference in the second shell's run must fail the comparison: the same
  # scenario with one mode (then one symlink target) changed is compared with the stock run.
  # Only the install half matters (the installed state is what is compared), so the planted
  # runs stop there.
  if [[ "$GROUP" == all || "$GROUP" == home ]]; then
  for plant in mode link; do
    STOP_AFTER=installed PLANT="$plant" scenario "$NEW_BASH" "plant$plant" novendor-home 0
    if diff "$TMP/state-stock-novendor-home.installed" "$TMP/state-plant$plant-novendor-home.installed" > "$TMP/diff.out"; then
      fail "a planted $plant change in the $NEW_BASH run was not detected by the end-state comparison"
    fi
  done
  pass "dynamic: a planted mode change and a planted symlink-target change in the second shell's run fail the comparison"
  fi
fi

printf 'install-bash32-test: %d checks passed\n' "$PASSED"
