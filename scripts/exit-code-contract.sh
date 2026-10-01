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
#   - It omitted `8` (worktree-gate refusal) entirely, AND every case here
#     ran from the CALLER's cwd rather than an isolated scratch one (task
#     6845 / bug 6896): `PLANAR_DB`/`HOME`/`PLANAR_WORKBENCH_ROOT` were
#     isolated but the working directory was not, so running this script
#     from inside a git worktree (as a coder cycle's own checkout often is)
#     made the worktree gate fire on the ordinary "missing required
#     argument" `task add` case BEFORE the parser ever ran, silently
#     reporting exit 8 in place of the expected 2. Every case below now
#     runs from a dedicated non-git scratch directory, and a NEW case
#     creates its own throwaway git repo + worktree to exercise the gate
#     for real.
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

# Resolve to ABSOLUTE paths before any case cd's into an isolated scratch
# working directory below (task 6845) -- a relative $bin_dir/$doc would
# stop resolving the moment the cwd changes.
bin_dir="$(cd "$bin_dir" && pwd)"
doc="$(cd "$(dirname "$doc")" && pwd)/$(basename "$doc")"

# Scratch DB *and* scratch HOME. `PLANAR_HOME` alone does NOT redirect the
# database: without PLANAR_DB the runtime falls back to ~/.planar/planar.db
# and auto-applies pending migrations, moving the operator's live schema.
# The queue's tables now live in planar.db itself (plan 1089), so PLANAR_DB is
# the only database the queue verbs open. PLANAR_AGENT_DB stays pinned for the
# arena tests that read this script, until plan 1089 M1 removes the pin; no
# binary reads it.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
export PLANAR_DB="$tmp/db.sqlite" PLANAR_AGENT_DB="$tmp/agent.db" HOME="$tmp/home" PLANAR_WORKBENCH_ROOT="$tmp/wb"
mkdir -p "$HOME"
# The WORKING DIRECTORY is isolated too (task 6845 / bug 6896), separately
# from PLANAR_DB/HOME/WORKBENCH above: a plain, non-git scratch directory.
# The cases invoke PLANNING verbs, which refuse outright (exit 8) when the
# cwd is inside a git worktree -- before the argument validation whose exit
# code is under test. Run from a worktree, `planar task add` returned 8
# where this gate wants 2, and the gate failed on WHERE it ran rather than
# on what the binary does.
#
# Master's plan-1033 M1 fixed the same bug by `cd "$tmp"` once; this keeps
# the per-case `(cd "$workdir" && ...)` form instead, because the
# worktree-gate case below must run from an EXPLICIT git worktree while
# every other case must not — one ambient cd cannot express both. The
# absolute-path resolution master paired with its fix is already done
# above, at the `bin_dir`/`doc` lines.
workdir="$tmp/work"
mkdir -p "$workdir"

# The queue verbs open this database and never create it, so a failed init
# would turn every queue case below into a 125 for the wrong reason. It is a
# hard failure (plan 1089, task 7129).
if ! init_out="$(cd "$workdir" && "$bin_dir/planar" init --name contract 2>&1)"; then
  printf 'exit-code-contract: setup failed: planar init exited non-zero:\n%s\n' "$init_out" >&2
  exit 2
fi
[ -f "$PLANAR_DB" ] || { printf 'exit-code-contract: setup failed: planar init left no database at %s\n' "$PLANAR_DB" >&2; exit 2; }

# The database fixtures the queue refusal cases need, built with python3 (the
# `sqlite3` CLI is not a dependency) from a copy of the freshly initialised
# database, BEFORE any case writes a queue row into it. The backup API copies a
# consistent image even if the source still has a WAL.
command -v python3 >/dev/null 2>&1 || { printf 'exit-code-contract: python3 is required to build the queue fixtures\n' >&2; exit 2; }
fixtures="$tmp/fixtures"
mkdir -p "$fixtures"
python3 - "$PLANAR_DB" "$fixtures" <<'PYEOF' || { printf 'exit-code-contract: setup failed: could not build the queue fixtures\n' >&2; exit 2; }
import os, sqlite3, sys

src_path, out = sys.argv[1], sys.argv[2]
newer = "insert into schema_migrations (version, description) values ((select max(version) + 1 from schema_migrations), 'newer')"

def build(name, *statements):
    source = sqlite3.connect(src_path)
    target = sqlite3.connect(os.path.join(out, name))
    source.backup(target)
    source.close()
    for statement in statements:
        target.execute(statement)
    target.commit()
    target.close()

# Behind: only the highest schema_migrations row is deleted, so the version
# reads head - 1 whichever migration is head.
build("behind.db", "delete from schema_migrations where version = (select max(version) from schema_migrations)")
# Ahead, and the queue tables need a newer build.
build("incompatible.db", newer,
      "insert into queue_schema (version, compat, description) values ((select max(version) + 1 from queue_schema), "
      "(select max(version) + 1 from queue_schema), 'needs a newer binary')")
# Ahead, and the queue marker still admits this binary.
build("ahead.db", newer,
      "insert into queue_schema (version, compat, description) values ((select max(version) + 1 from queue_schema), "
      "1, 'compatible newer marker')")
# Equal version, but two branches shipped different migrations under it.
build("foreign.db", "drop table queue_schema")
PYEOF

failures=0
checked=0
declare -a expected_codes=()

# case: <expected> <label> <binary> <argv...> — runs from the isolated,
# non-git `$workdir`.
check() {
  local want="$1"; shift
  local label="$1"; shift
  local bin="$1"; shift
  (cd "$workdir" && "$bin_dir/$bin" "$@" >/dev/null 2>&1)
  local got=$?
  checked=$((checked + 1))
  expected_codes+=("$want")
  if [ "$got" -ne "$want" ]; then
    printf 'MISMATCH  %-46s want %-3s got %s   (%s %s)\n' "$label" "$want" "$got" "$bin" "$*"
    failures=$((failures + 1))
  fi
}

# case: <expected> <label> <cwd> <binary> <argv...> — runs from an
# EXPLICIT cwd rather than `$workdir`. Only the worktree-gate case below
# needs this; every other case wants the plain isolated directory.
check_in() {
  local want="$1"; shift
  local label="$1"; shift
  local dir="$1"; shift
  local bin="$1"; shift
  (cd "$dir" && "$bin_dir/$bin" "$@" >/dev/null 2>&1)
  local got=$?
  checked=$((checked + 1))
  expected_codes+=("$want")
  if [ "$got" -ne "$want" ]; then
    printf 'MISMATCH  %-46s want %-3s got %s   (%s %s)\n' "$label" "$want" "$got" "$bin" "$*"
    failures=$((failures + 1))
  fi
}

# case: <expected> <label> <binary> <env args...> -- <argv...> — runs one verb
# from `$workdir` under extra `env` arguments (a different PLANAR_DB, or
# `-u VAR` to remove a variable). Used by the queue store cases below, whose
# codes are checked against the queue tables (direction 2b/2c), not the general
# table, so it does not record into `expected_codes`.
fcheck() {
  local want="$1" label="$2" bin="$3"; shift 3
  local -a envargs=()
  while [ $# -gt 0 ] && [ "$1" != "--" ]; do envargs+=("$1"); shift; done
  shift
  (cd "$workdir" && env ${envargs[@]+"${envargs[@]}"} "$bin_dir/$bin" "$@" >/dev/null 2>&1)
  local got=$?
  checked=$((checked + 1))
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

# --- the worktree gate (task 6845): a planning verb run from inside a git
# worktree exits 8, BEFORE the parser even runs -- so this fires even
# though `task add` with no flags would otherwise be the exit-2 "missing
# required argument" case above. A throwaway git repo + worktree, entirely
# under $tmp, never the real checkout.
git_repo="$tmp/gitrepo"
mkdir -p "$git_repo"
git -C "$git_repo" init -q -b main >/dev/null 2>&1
git -C "$git_repo" -c user.email=contract@example.com -c user.name=contract \
  commit -q --allow-empty -m init >/dev/null 2>&1
worktree_path="$tmp/gitrepo-wt"
git -C "$git_repo" worktree add -q -b exit-code-contract-wt "$worktree_path" main >/dev/null 2>&1

check_in 8 "planar: worktree-gate refusal" "$worktree_path" planar task add

# --- `planar-agent queue run` (plan 1080, task 7013, decision 1188) ---------
# The command's own status passes through, and the queue owns a handful of
# codes. Each case runs the real binary in the scratch env above (its own
# PLANAR_AGENT_DB, HOME and cwd), so no case can reach the operator's
# ~/.planar. `qcheck` records the code under the row of the QUEUE table it
# belongs to, so direction 2 checks that table and not the general one.
declare -a queue_rows=()
qcheck() { # <expected> <table-row> <label> <env args...> -- <queue run argv...>
  local want="$1" row="$2" label="$3"; shift 3
  local -a envargs=()
  while [ $# -gt 0 ] && [ "$1" != "--" ]; do envargs+=("$1"); shift; done
  shift
  (cd "$workdir" && env ${envargs[@]+"${envargs[@]}"} "$bin_dir/planar-agent" queue run "$@" >/dev/null 2>&1)
  local got=$?
  checked=$((checked + 1))
  queue_rows+=("$row")
  if [ "$got" -ne "$want" ]; then
    printf 'MISMATCH  %-46s want %-3s got %s   (queue run %s)\n' "$label" "$want" "$got" "$*"
    failures=$((failures + 1))
  fi
}
noexec_file="$tmp/not-executable"
: > "$noexec_file"
chmod 644 "$noexec_file"
qcheck 1   1   "queue run: no command (parse failure)"       --
qcheck 1   1   "queue run: unknown flag (parse failure)"     -- --no-such-flag -- true
qcheck 2   2   "queue run: model launcher refused by guard"  -- -- claude
qcheck 2   2   "queue run: invalid --timeout duration"       -- --timeout 0 -- true
qcheck 124 124 "queue run: --timeout overrun"                -- --timeout 1s -- sleep 30
qcheck 126 126 "queue run: not executable"                   -- -- "$noexec_file"
qcheck 127 127 "queue run: program not found"                -- -- exit-code-contract-no-such-program
qcheck 143 128 "queue run: terminated by SIGTERM (128+N)"    -- -- sh -c 'kill -TERM $$'
qcheck 7   own "queue run: pass-through status"              -- -- sh -c 'exit 7'

# --- `planar-agent queue cancel` (plan 1080, task 7023) ----------------------
# Only what needs no live process: the entries the cases above ended (1 is the
# `--timeout` overrun, so its history row exists), a number nothing issued, a
# bad number, and (below) an unreachable store. Cancelling a waiting or a
# running entry needs a live submitter and is pinned by queue_cancel.t.cpp.
declare -a cancel_rows=()
ccheck() { # <expected> <table-row> <label> -- <queue cancel argv...>
  local want="$1" row="$2" label="$3"; shift 3
  shift # the --
  (cd "$workdir" && "$bin_dir/planar-agent" queue cancel "$@" >/dev/null 2>&1)
  local got=$?
  checked=$((checked + 1))
  cancel_rows+=("$row")
  if [ "$got" -ne "$want" ]; then
    printf 'MISMATCH  %-46s want %-3s got %s   (queue cancel %s)\n' "$label" "$want" "$got" "$*"
    failures=$((failures + 1))
  fi
}
# The first entry of a fresh planar.db is 1,000,001: the queue migration seeds
# the sequence counter at 1,000,000. Case "--timeout overrun" above was it.
first_seq=1000001
ccheck 6 6 "queue cancel: entry already ended"               -- "$first_seq"
ccheck 1 1 "queue cancel: no such entry"                     -- 999999
ccheck 1 1 "queue cancel: no argument (parse failure)"       --
ccheck 2 2 "queue cancel: not a positive integer"            -- abc
ccheck 2 2 "queue cancel: zero"                              -- 0
# --- the queue's store is planar.db: every refusal is 125 --------------------
# Run last, because the cases above fill the real database. Each of the three
# verbs runs against each fixture (built with python3 above); the fixtures are
# separate files, so no case can change another's. `planar-watch queue` reads
# the same file and exits 7 on a version mismatch in either direction.
for verb in run cancel status; do
  case "$verb" in
    run)    argv=(-- true) ;;
    cancel) argv=(1) ;;
    status) argv=(1) ;;
  esac
  fcheck 125 "queue $verb: behind planar.db"                planar-agent PLANAR_DB="$fixtures/behind.db"       -- queue "$verb" "${argv[@]}"
  fcheck 125 "queue $verb: incompatible ahead planar.db"    planar-agent PLANAR_DB="$fixtures/incompatible.db" -- queue "$verb" "${argv[@]}"
  fcheck 125 "queue $verb: equal version, no queue tables"  planar-agent PLANAR_DB="$fixtures/foreign.db"      -- queue "$verb" "${argv[@]}"
  fcheck 125 "queue $verb: neither PLANAR_DB nor HOME"      planar-agent -u PLANAR_DB -u HOME                  -- queue "$verb" "${argv[@]}"
  mkdir "$tmp/store-dir"
  fcheck 125 "queue $verb: PLANAR_DB is a directory"        planar-agent PLANAR_DB="$tmp/store-dir"            -- queue "$verb" "${argv[@]}"
  rmdir "$tmp/store-dir"
done
# A compatible ahead database is used as it is.
fcheck 0   "queue run: compatible ahead planar.db"          planar-agent PLANAR_DB="$fixtures/ahead.db"        -- queue run -- true
fcheck 7   "planar-watch queue: ahead planar.db"            planar-watch PLANAR_DB="$fixtures/ahead.db"        -- queue
fcheck 7   "planar-watch queue: behind planar.db"           planar-watch PLANAR_DB="$fixtures/behind.db"       -- queue
# The 125 rows of the queue tables must name these reasons.
store_reasons=(schema_version_behind queue_schema_incompatible queue_schema_foreign)

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

# --- direction 2b: the `planar-agent queue run` table ------------------------
# Its own section, so a code that is only documented in the general table
# above does not satisfy it. The rows a case is filed under must all exist,
# and each row must say what the code means (a swapped meaning fails).
queue_section="$(sed -n '/^#### Queue run exit codes/,/^#\{1,4\} /p' "$doc")"
if [ -z "$queue_section" ]; then
  printf 'exit-code-contract: could not find the "Queue run exit codes" section in %s\n' "$doc" >&2
  exit 1
fi
queue_meaning() { # <row> -> a grep -E pattern the row's line must match
  case "$1" in
    1)   printf 'parse failure' ;;
    2)   printf 'refused|guard' ;;
    124) printf 'run limit' ;;
    125) printf 'queue failed' ;;
    126) printf 'could not be executed' ;;
    127) printf 'not found' ;;
    128) printf 'terminated by signal' ;;
    *)   printf '.' ;;
  esac
}
for row in $(printf '%s\n' "${queue_rows[@]}" | sort -u); do
  if [ "$row" = "own" ]; then
    printf '%s\n' "$queue_section" | grep -qF "the command's own exit status" || {
      printf 'UNDOCUMENTED  the queue run table does not say the command'"'"'s own exit status passes through\n'
      failures=$((failures + 1)); }
    continue
  fi
  line="$(printf '%s\n' "$queue_section" | grep -E "^\| \`$row\`" | head -n 1)"
  if [ -z "$line" ]; then
    printf 'UNDOCUMENTED  exit %s is returned by a checked queue run case but has no row in the queue run table of %s\n' "$row" "$doc"
    failures=$((failures + 1))
  elif ! printf '%s\n' "$line" | grep -qiE "$(queue_meaning "$row")"; then
    printf 'MISDOCUMENTED  queue run row %s does not carry its meaning (%s): %s\n' "$row" "$(queue_meaning "$row")" "$line"
    failures=$((failures + 1))
  fi
done
if ! printf '%s\n' "$queue_section" | grep -qF 'outside 0..255'; then
  printf 'UNDOCUMENTED  the queue run table does not say a status outside 0..255 exits 125\n'
  failures=$((failures + 1))
fi

for reason in "${store_reasons[@]}"; do
  if ! printf '%s\n' "$queue_section" | grep -E '^\| `125`' | grep -qF "$reason"; then
    printf 'UNDOCUMENTED  the queue run table'"'"'s 125 row does not name %s\n' "$reason"
    failures=$((failures + 1))
  fi
done

# --- direction 2c: the `planar-agent queue cancel` table ---------------------
cancel_section="$(sed -n '/^#### Queue cancel exit codes/,/^#\{1,4\} /p' "$doc")"
if [ -z "$cancel_section" ]; then
  printf 'exit-code-contract: could not find the "Queue cancel exit codes" section in %s\n' "$doc" >&2
  exit 1
fi
cancel_meaning() { # <row> -> a grep -E pattern the row's line must match
  case "$1" in
    0)   printf 'cancelled' ;;
    1)   printf 'no such entry' ;;
    2)   printf 'refused input' ;;
    6)   printf 'already ended' ;;
    125) printf 'queue failed' ;;
    *)   printf '.' ;;
  esac
}
for row in 0 $(printf '%s\n' "${cancel_rows[@]}" | sort -u); do
  line="$(printf '%s\n' "$cancel_section" | grep -E "^\| \`$row\`" | head -n 1)"
  if [ -z "$line" ]; then
    printf 'UNDOCUMENTED  exit %s is returned by a checked queue cancel case but has no row in the queue cancel table of %s\n' "$row" "$doc"
    failures=$((failures + 1))
  elif ! printf '%s\n' "$line" | grep -qiE "$(cancel_meaning "$row")"; then
    printf 'MISDOCUMENTED  queue cancel row %s does not carry its meaning (%s): %s\n' "$row" "$(cancel_meaning "$row")" "$line"
    failures=$((failures + 1))
  fi
done

for reason in "${store_reasons[@]}"; do
  if ! printf '%s\n' "$cancel_section" | grep -E '^\| `125`' | grep -qF "$reason"; then
    printf 'UNDOCUMENTED  the queue cancel table'"'"'s 125 row does not name %s\n' "$reason"
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
