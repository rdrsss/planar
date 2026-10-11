#!/usr/bin/env bash
# The retired Makefile install channel (plan 1122, task rel-retire-make-install; tech spec
# 677 "The retired channel"). Asserts:
#   - make install, install-bin, install-full, uninstall and uninstall-full each exit
#     non-zero with make's "No rule to make target" message (run with -n, so nothing can
#     install even if a target came back), and the PREFIX variable is gone;
#   - `make help` lists none of them;
#   - INSTALL.md, CLAUDE.md, docs/architecture.md and docs/toolchain-parity.md carry no
#     `make install`, `make install-full`, `make uninstall` or `make uninstall-full`.
# Read-only: no build, no network, nothing outside the repository is touched.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
fail() { printf 'install-retired-targets-test: %s\n' "$*" >&2; exit 1; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

for t in install install-bin install-full uninstall uninstall-full; do
  rc=0
  make -n "$t" >"$TMP/out" 2>"$TMP/err" || rc=$?
  [[ "$rc" -ne 0 ]] || fail "target-$t-still-exists: make -n $t exited 0: $(cat "$TMP/out")"
  grep -Eq "No rule to make target .${t}[.'\`]" "$TMP/err" \
    || fail "target-$t-no-rule-message: make -n $t did not say No rule to make target: $(cat "$TMP/err")"
done

make help >"$TMP/help" 2>&1 || fail "make help failed: $(cat "$TMP/help")"
grep -Eq '^ *(.\[[0-9;]*m)?(install|install-bin|install-full|uninstall|uninstall-full)(.\[[0-9;]*m)? ' "$TMP/help" \
  && fail "help-lists-retired-target: $(grep -E 'install' "$TMP/help")"
grep -Eq '^[[:space:]]*PREFIX[[:space:]]*(\?|:)?=' Makefile && fail "prefix-variable-still-defined: Makefile defines PREFIX"

for d in INSTALL.md CLAUDE.md docs/architecture.md docs/toolchain-parity.md; do
  if grep -En 'make (install|uninstall)' "$d" >"$TMP/hit"; then
    fail "doc-has-make-install-recipe: $d: $(cat "$TMP/hit")"
  fi
done
printf 'install retired targets tests: ok\n'
