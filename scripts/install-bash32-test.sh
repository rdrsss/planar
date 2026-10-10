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
#           and compared tree for tree (run-specific lock records left out, the HOME
#           path normalised) between two runs under /bin/bash (a self-check that keeps
#           the comparison honest) and, when a different, newer bash is on the host,
#           with that bash's run.
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

if [[ "${1-}" != "--dynamic-only" ]]; then
  lint_selftest
  pass "static lint self-test: planted unguarded and bash-4 constructs are detected"
  static_check
  pass "static lint: no unguarded array expansion or bash-4-only construct in install.sh and its sourced scripts"
fi
[[ "${1-}" == "--static-only" ]] && { printf 'install-bash32-test: %d checks passed (static only)\n' "$PASSED"; exit 0; }

# ---- dynamic half ---------------------------------------------------------------------

BASH_BIN=/bin/bash
[[ -x "$BASH_BIN" ]] || fail "dynamic: $BASH_BIN is not executable"
child_major="$("$BASH_BIN" -c 'echo "${BASH_VERSINFO[0]}"')"
if [[ "$(uname -s)" == Darwin ]]; then
  [[ "$child_major" == 3 ]] || fail "dynamic: /bin/bash on macOS must be bash 3.2 (BASH_VERSINFO[0]=$child_major); stock macOS ships 3.2"
  "$BASH_BIN" --version | head -n 1 | grep -q 'version 3\.2' || fail "dynamic: $BASH_BIN --version is not 3.2"
  pass "dynamic: /bin/bash is bash 3.2 (BASH_VERSINFO[0]=3)"
else
  printf 'note: skipping the "/bin/bash is 3.2" assertion: %s ships /bin/bash %s, not 3.2; the install and uninstall still run under it\n' "$(uname -s)" "$child_major"
fi

# A newer bash for the end-state comparison, when the host has one.
NEW_BASH=""
for c in /opt/homebrew/bin/bash /usr/local/bin/bash "$(command -v bash || true)"; do
  [[ -n "$c" && -x "$c" ]] || continue
  # the same file as /bin/bash (Debian: /bin -> /usr/bin) is no comparison at all
  [[ "$c" -ef /bin/bash ]] && continue
  m="$("$c" -c 'echo "${BASH_VERSINFO[0]}"' 2>/dev/null || true)"
  if [[ "$m" =~ ^[0-9]+$ && "$m" -gt "$child_major" && "$m" -ge 4 ]]; then NEW_BASH="$c"; break; fi
done
if [[ -n "$NEW_BASH" ]]; then
  printf 'note: comparing end states against %s (bash %s)\n' "$NEW_BASH" "$("$NEW_BASH" -c 'echo "${BASH_VERSINFO[0]}"')"
else
  printf 'note: no bash 4 or newer other than /bin/bash on this host; the end state is checked against explicit expectations only\n'
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

# tree_state HOME -- the relative file list plus checksums, for comparing runs. Run-specific
# state is normalised: .planar.lock/ (owner and released records) is left out, and the
# HOME path is replaced by @HOME@ inside every file before it is checksummed (the
# manifest and release stamp record absolute paths).
tree_state() { ( cd "$1" && find . -path ./.planar.lock -prune -o \( -type f -o -type l \) -print | sort | while IFS= read -r f; do
  if [[ -L "$f" ]]; then printf '%s -> link\n' "$f"; else printf '%s %s\n' "$f" "$(LC_ALL=C sed "s#$1#@HOME@#g" "$f" | cksum)"; fi
done ); }

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
  tree_state "$home" > "$TMP/state-$tag-$name.installed"
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
  tree_state "$home" > "$TMP/state-$tag-$name.removed"
}

scenario "$BASH_BIN" stock novendor-flag 2 --no-vendor
pass "dynamic: prebuilt install (--no-vendor, vendors present) and uninstall exit 0 under $BASH_BIN"
scenario "$BASH_BIN" stock novendor-home 0
pass "dynamic: prebuilt install and uninstall exit 0 under $BASH_BIN with no vendor present (empty VENDORS_FOUND)"
scenario "$BASH_BIN" stock vendors 1
pass "dynamic: prebuilt install and uninstall exit 0 under $BASH_BIN with vendors present"

# compare_runs TAG_A TAG_B LABEL -- the three scenarios' end states under two tags must be
# identical once the scenario tag in the HOME path is normalised.
compare_runs() {
  local a="$1" b="$2" label="$3" s ph
  for s in novendor-flag novendor-home vendors; do
    for ph in installed removed; do
      diff "$TMP/state-$a-$s.$ph" "$TMP/state-$b-$s.$ph" > "$TMP/diff.out" \
        || fail "[$s/$ph] end state differs between $label: $(cat "$TMP/diff.out")"
    done
  done
}

# Self-check: the same bash run twice must compare equal, whether or not the host has a
# newer bash, so the comparison itself cannot rot unnoticed.
run_scenarios() { scenario "$1" "$2" novendor-flag 2 --no-vendor; scenario "$1" "$2" novendor-home 0; scenario "$1" "$2" vendors 1; }
run_scenarios "$BASH_BIN" again
compare_runs stock again "two runs under $BASH_BIN"
pass "dynamic: comparison self-check: two runs under $BASH_BIN have identical end states"

if [[ -n "$NEW_BASH" ]]; then
  run_scenarios "$NEW_BASH" new
  compare_runs stock new "$BASH_BIN and $NEW_BASH"
  pass "dynamic: end states under $BASH_BIN and $NEW_BASH are identical"
fi

printf 'install-bash32-test: %d checks passed\n' "$PASSED"
