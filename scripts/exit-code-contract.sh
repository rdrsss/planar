#!/usr/bin/env bash
#
# exit-code-contract.sh — prove the DOCUMENTED exit codes are the codes the
# binaries actually return (plan 996, tasks 6813/6814).
#
# ## Why this exists
#
# `docs/cli-reference.md` § Exit Codes is a CONTRACT: skills and scripts
# branch on it to decide whether a failure is the operator's fault, a
# system fault, or a health verdict. Nothing checked it, and it drifted
# badly enough to invert that decision:
#
#   - It claimed `1` for a missing argument and `64` for an unknown flag.
#     Both are `2`. Its gloss on `2` was "System error: database open
#     failure, I/O error", so a typo read as a system fault.
#   - It documented `64` as EX_USAGE. `64` means NOT IMPLEMENTED; nothing
#     emits it for a usage error.
#   - It omitted `5` (cross-scope refused), `6` (slug conflict) and `7`
#     (schema ahead) entirely — while the same file's cross-scope-guard
#     section referenced `5`.
#   - `pl-health` mapped exit 2 to `critical`, so `planar health --typo`
#     reported a CRITICAL system (task 6814).
#
# Every existing gate stayed green throughout: `cli_usage_lint` checks that
# authored surfaces never name an unexposed FLAG, `surface_lint` checks
# links and command shapes, and the `*_parity.t.cpp` pins cover specific
# leaves. None of them compares a documented CONVENTION against observed
# behaviour.
#
# ## What it checks
#
# Two directions, because the drift can come from either side:
#
#   1. BEHAVIOUR — each case below is run for real and its exit code must
#      equal the expected one.
#   2. DOCUMENTATION — every code these cases expect must appear in the
#      Exit Codes table. A code the binaries emit but the table omits is a
#      failure, which is how `5`/`6`/`7` went missing.
#
# The per-binary split is deliberate and is asserted here rather than
# smoothed over: a parse failure is `2` on `planar` and `1` on the other
# three. See `src/cmd/planar-watch/exit.cppm`'s header for why a shared,
# binary-parameterized helper was removed.
#
# ## Usage
#
#   scripts/exit-code-contract.sh [--bin-dir DIR] [--doc PATH]

set -uo pipefail

bin_dir="build/debug/bin"
doc="docs/cli-reference.md"
while [ $# -gt 0 ]; do
  case "$1" in
    --bin-dir) bin_dir="${2:?--bin-dir needs a path}"; shift 2 ;;
    --doc)     doc="${2:?--doc needs a path}"; shift 2 ;;
    -h|--help) sed -n '2,48p' "$0"; exit 0 ;;
    *) printf 'exit-code-contract: unknown argument %s\n' "$1" >&2; exit 2 ;;
  esac
done

for b in planar planar-agent planar-watch planar-ext; do
  [ -x "$bin_dir/$b" ] || { printf 'exit-code-contract: missing %s/%s — build first\n' "$bin_dir" "$b" >&2; exit 2; }
done
[ -f "$doc" ] || { printf 'exit-code-contract: no doc at %s\n' "$doc" >&2; exit 2; }

# Scratch DB *and* scratch HOME. `PLANAR_HOME` alone does NOT redirect the
# database: without PLANAR_DB the runtime falls back to ~/.planar/planar.db
# and auto-applies pending migrations, moving the operator's live schema.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
export PLANAR_DB="$tmp/db.sqlite" HOME="$tmp/home" PLANAR_WORKBENCH_ROOT="$tmp/wb"
mkdir -p "$HOME"
"$bin_dir/planar" init --name contract >/dev/null 2>&1 || true

failures=0
checked=0
declare -a expected_codes=()

# case: <expected> <label> -- <binary> <argv...>
check() {
  local want="$1"; shift
  local label="$1"; shift
  local bin="$1"; shift
  "$bin_dir/$bin" "$@" >/dev/null 2>&1
  local got=$?
  checked=$((checked + 1))
  expected_codes+=("$want")
  if [ "$got" -ne "$want" ]; then
    printf 'MISMATCH  %-46s want %-3s got %s   (%s %s)\n' "$label" "$want" "$got" "$bin" "$*"
    failures=$((failures + 1))
  fi
}

# --- `planar`: parse failures are 2 ----------------------------------------
check 2 "planar: unknown flag"              planar plan list --no-such-flag
check 2 "planar: unknown subcommand"        planar no-such-verb
check 2 "planar: missing required argument" planar task add
check 2 "planar: missing flag value"        planar plan list --status
check 1 "planar: entity not found"          planar plan show 987654321

# --- the other three: parse failures are 1, NOT 2 --------------------------
check 1 "planar-agent: unknown flag"        planar-agent --no-such-flag
check 1 "planar-watch: unknown flag"        planar-watch --no-such-flag
check 1 "planar-ext: unknown flag"          planar-ext --no-such-flag

# --- the removed-verb stubs -------------------------------------------------
check 2 "planar: removed verb (scope use)"  planar scope use anything

printf 'exit-code-contract: %d behaviour cases checked\n' "$checked"

# --- direction 2: every expected code must be DOCUMENTED -------------------
doc_codes="$(sed -n '/^### Exit Codes/,/^## /p' "$doc" | grep -oE '^\| `[0-9]+`' | grep -oE '[0-9]+' | sort -u)"
if [ -z "$doc_codes" ]; then
  printf 'exit-code-contract: could not find the Exit Codes table in %s\n' "$doc" >&2
  exit 1
fi
for want in $(printf '%s\n' "${expected_codes[@]}" | sort -u); do
  if ! printf '%s\n' "$doc_codes" | grep -qx "$want"; then
    printf 'UNDOCUMENTED  exit %s is returned by a checked case but is absent from %s\n' "$want" "$doc"
    failures=$((failures + 1))
  fi
done

if [ "$failures" -ne 0 ]; then
  printf 'exit-code-contract: FAILED — %d problem(s).\n' "$failures" >&2
  printf 'The binaries are oracle-matched; prefer fixing the TABLE in %s.\n' "$doc" >&2
  printf 'Authoritative source: src/cmd/planar/exit.cppm\n' >&2
  exit 1
fi
printf 'exit-code-contract: OK — behaviour matches %s\n' "$doc"
