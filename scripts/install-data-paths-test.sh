#!/usr/bin/env bash
# Data-path fixtures (plan 1122, task rel-data-paths). The one list of data
# paths lives in scripts/install-lib/data-paths.sh; install.sh sources it so that
# neither an install nor `--uninstall` removes a listed path, and INSTALL.md's
# "Preserved paths" section is generated from it. This test:
#   - checks the sourced API (order, resolution, relocation, predicates);
#   - checks INSTALL.md matches the list, and that the comparison catches a
#     removal from one side only (run on scratch copies) and passes when both
#     sides drop the entry;
#   - drives install.sh (stub cmake, scratch HOME) over a prefix seeded with
#     every data path, with cleanup entries aimed at each of them, and asserts
#     checksums are unchanged after the install, a --force install, and
#     --uninstall (also --uninstall --force);
#   - drives a relocated database and workbench: named, left untouched;
#   - drives a relocated data path inside a managed tree: the install stops.
# Nothing real is touched: HOME and every variable that could point elsewhere are
# scratch paths. Runs under stock bash 3.2.
# shellcheck disable=SC2016,SC2088,SC2012,SC2030,SC2031  # literal backticks and ~ in fixtures; ls for messages
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
fail() { printf 'install-data-paths-test: %s\n' "$*" >&2; exit 1; }
PASSED=0
pass() { PASSED=$((PASSED + 1)); }
# has_line TEXT LINE -- LINE is one whole line of TEXT (no pipe, so no SIGPIPE under pipefail).
has_line() { [[ $'\n'"$1"$'\n' == *$'\n'"$2"$'\n'* ]]; }

# --- the sourced API -----------------------------------------------------------

export HOME="$TMP/apihome"
mkdir -p "$HOME/.planar"
unset PLANAR_DB PLANAR_CONFIG_PATH PLANAR_WORKBENCH_ROOT PLANAR_LOCAL_HOME PLANAR_TEMPLATES_DIR
# shellcheck source=scripts/install-lib/data-paths.sh
source "$ROOT/scripts/install-lib/data-paths.sh"

want_names="planar.db planar.db-wal planar.db-shm queue-logs retired workbench config.toml local workspaces models execute templates"
[[ "$(planar_data_path_names | tr '\n' ' ' | sed 's/ $//')" == "$want_names" ]] \
  || fail "data path names or order differ from the spec: $(planar_data_path_names | tr '\n' ' ')"
pass

P="$HOME/.planar"
[[ -z "$(planar_data_paths_report "$P")" ]] || fail "a report was printed with nothing relocated"
has_line "$(planar_data_paths_list "$P")" "config.toml|file|$P/config.toml|" || fail "default config.toml location wrong"
pass

for p in "$P/planar.db" "$P/planar.db-wal" "$P/planar.db-shm" "$P/queue-logs" "$P/queue-logs/7.log" "$P/retired/a/b" \
         "$P/workbench/x/README.md" "$P/config.toml" "$P/local/skills/mine/SKILL.md" "$P/workspaces/w.json" \
         "$P/models/catalog.json" "$P/execute/default/profile.toml" "$P/templates/a.toml"; do
  planar_is_data_path "$P" "$p" || fail "$p is not recognised as a data path"
done
for p in "$P/bin" "$P/bin/planar" "$P/scripts" "$P/planar.dbx" "$P/queue-logs-old" "$P/agent.db" "$P/config.toml.bak" "$P/skills/planar"; do
  ! planar_is_data_path "$P" "$p" || fail "$p is wrongly recognised as a data path"
done
for p in "$P" "$P/." "$P/../.planar" "/"; do
  planar_covers_data_path "$P" "$p" || fail "$p should cover a data path"
done
! planar_covers_data_path "$P" "$P/bin" || fail "bin/ wrongly covers a data path"
# Lexical tricks resolve before comparison.
planar_removal_blocked "$P" "$P/bin/../workbench/x" || fail "a .. path into workbench/ was not blocked"
[[ "$PLANAR_DATA_PATH_HIT" == "workbench" ]] || fail "wrong hit name: $PLANAR_DATA_PATH_HIT"
# A symlink into a data path is the data path.
ln -s "$P/models" "$P/bin-link" 2>/dev/null || { mkdir -p "$P/models"; ln -s "$P/models" "$P/bin-link"; }
planar_is_data_path "$P" "$P/bin-link/x" || fail "a symlink into models/ was not recognised"
rm -f "$P/bin-link"
pass

# Relocation: each variable, ~ expansion, the default-location non-relocation.
(
  export PLANAR_DB="$TMP/ext/p.db" PLANAR_WORKBENCH_ROOT="$TMP/ext/wb" PLANAR_CONFIG_PATH='~/cfg/c.toml'
  export PLANAR_LOCAL_HOME="$TMP/ext/lh" PLANAR_TEMPLATES_DIR="$P/templates"
  l="$(planar_data_paths_list "$P")"
  for line in "planar.db|file|$TMP/ext/p.db|PLANAR_DB" "planar.db-wal|file|$TMP/ext/p.db-wal|PLANAR_DB" \
              "planar.db-shm|file|$TMP/ext/p.db-shm|PLANAR_DB" "workbench|dir|$TMP/ext/wb|PLANAR_WORKBENCH_ROOT" \
              "config.toml|file|$HOME/cfg/c.toml|PLANAR_CONFIG_PATH" "local|dir|$TMP/ext/lh/.planar/local|PLANAR_LOCAL_HOME" \
              "templates|dir|$P/templates|"; do
    has_line "$l" "$line" || { printf 'missing list line: %s\n%s\n' "$line" "$l" >&2; exit 1; }
  done
  r="$(planar_data_paths_report "$P")"
  for n in "planar.db (and its -wal and -shm sidecars) is relocated by PLANAR_DB to $TMP/ext/p.db" \
           "data path workbench is relocated by PLANAR_WORKBENCH_ROOT" "data path config.toml is relocated by PLANAR_CONFIG_PATH" \
           "data path local is relocated by PLANAR_LOCAL_HOME"; do
    [[ "$r" == *"$n"* ]] || { printf 'report lacks: %s\n%s\n' "$n" "$r" >&2; exit 1; }
  done
  [[ "$r" != *"data path templates"* ]] || { echo 'templates at the default location was reported as relocated' >&2; exit 1; }
  [[ "$r" != *"planar.db-wal"* ]] || { echo 'sidecars were reported separately' >&2; exit 1; }
  planar_is_data_path "$P" "$TMP/ext/p.db-wal" || exit 2
  planar_is_data_path "$P" "$TMP/ext/wb/p1/x.md" || exit 3
  planar_is_data_path "$P" "$TMP/ext/lh/.planar/local/skills" || exit 4
  planar_is_data_path "$P" "$HOME/cfg/c.toml" || exit 5
  # The default location stays protected even while relocated.
  planar_is_data_path "$P" "$P/workbench/x" || exit 6
  # An empty variable relocates nothing.
  has_line "$(PLANAR_DB="" planar_data_paths_list "$P")" "planar.db|file|$P/planar.db|" || exit 7
) || fail "relocation check failed ($?)"
pass

# A data path with no relocation variable (queue-logs/, retired/, ...) resolves
# through _planar_dp_value with an empty variable name. Bash 4 and later reject
# `${!1}` for an empty name ("invalid variable name"), which fired install.sh's
# ERR trap on Linux; the answer must be empty, silent and successful everywhere.
dp_err="$(bash -c 'set -eEuo pipefail; trap "echo ERR-TRAP >&2" ERR; source "$1"; v="$(_planar_dp_value "")"; [ -z "$v" ]; l="$(_planar_dp_location /r queue-logs "")"; [ "$l" = /r/queue-logs ]' dp "$ROOT/scripts/install-lib/data-paths.sh" 2>&1 >/dev/null)" \
  || fail "a data path without a relocation variable did not resolve: $dp_err"
[[ -z "$dp_err" ]] || fail "resolving a data path without a relocation variable printed: $dp_err"
pass

# The default location is $HOME/.planar even when the install root is elsewhere:
# both are protected, and a variable naming $HOME/.planar/... is no relocation.
ALT="$TMP/altroot/inst"
mkdir -p "$ALT"
(
  for p in "$ALT/workbench/x" "$HOME/.planar/workbench/x" "$HOME/.planar/planar.db-wal" "$HOME/.planar/templates/a.toml" \
           "$HOME/.planar/local/skills"; do
    planar_is_data_path "$ALT" "$p" || { echo "$p is not protected under a root elsewhere" >&2; exit 1; }
  done
  ! planar_is_data_path "$ALT" "$HOME/.planar/bin" || exit 2
  export PLANAR_WORKBENCH_ROOT="$HOME/.planar/workbench" PLANAR_DB="$HOME/.planar/planar.db" PLANAR_TEMPLATES_DIR='~/.planar/templates'
  export PLANAR_CONFIG_PATH="$HOME/.planar/config.toml" PLANAR_LOCAL_HOME="$HOME"
  [[ -z "$(planar_data_paths_report "$ALT")" ]] || { echo "a $HOME/.planar location was reported as relocated: $(planar_data_paths_report "$ALT")" >&2; exit 3; }
  # A real relocation from a root elsewhere is still reported.
  r="$(PLANAR_WORKBENCH_ROOT="$TMP/ext/wb2" planar_data_paths_report "$ALT")"
  [[ "$r" == *"data path workbench is relocated by PLANAR_WORKBENCH_ROOT to $TMP/ext/wb2"* ]] || exit 4
  # PLANAR_DB is read literally by the runtime: no ~ expansion, relative to the cwd.
  l="$(cd "$TMP" && PLANAR_DB='~/x.db' planar_data_paths_list "$ALT")"
  has_line "$l" "planar.db|file|$TMP/~/x.db|PLANAR_DB" || { printf 'PLANAR_DB ~ was expanded or mislocated:\n%s\n' "$l" >&2; exit 5; }
  # PLANAR_LOCAL_HOME is literal too; PLANAR_WORKBENCH_ROOT expands ~.
  l="$(cd "$TMP" && PLANAR_LOCAL_HOME='~/lh' PLANAR_WORKBENCH_ROOT='~/wb3' planar_data_paths_list "$ALT")"
  has_line "$l" "local|dir|$TMP/~/lh/.planar/local|PLANAR_LOCAL_HOME" || { printf 'PLANAR_LOCAL_HOME ~ expanded:\n%s\n' "$l" >&2; exit 6; }
  has_line "$l" "workbench|dir|$HOME/wb3|PLANAR_WORKBENCH_ROOT" || { printf 'PLANAR_WORKBENCH_ROOT ~ not expanded:\n%s\n' "$l" >&2; exit 7; }
) || fail "default-location resolution check failed ($?)"
pass

# planar_root_has_data_path: a listed data path is evidence, unknown files are not.
mkdir -p "$TMP/evid/none" "$TMP/evid/cfg" "$TMP/evid/mixed"
printf 'x\n' > "$TMP/evid/none/thesis.txt"
printf '[x]\n' > "$TMP/evid/cfg/config.toml"
printf 'x\n' > "$TMP/evid/mixed/thesis.txt"; mkdir -p "$TMP/evid/mixed/workbench"
! planar_root_has_data_path "$TMP/evid/none" || fail "unknown files alone were taken as ownership evidence"
planar_root_has_data_path "$TMP/evid/cfg" || fail "config.toml was not taken as ownership evidence"
planar_root_has_data_path "$TMP/evid/mixed" || fail "workbench/ beside unknown files was not taken as ownership evidence"
mkdir -p "$TMP/evid/inroot/db"; printf 'x\n' > "$TMP/evid/inroot/db/p.db"
PLANAR_DB="$TMP/evid/inroot/db/p.db" planar_root_has_data_path "$TMP/evid/inroot" || fail "a relocated database inside the root was not evidence"
pass

# --- INSTALL.md matches the list ---------------------------------------------------

# doc_matches DATAPATHS_SH INSTALL_MD -- status 0 when the generated list equals
# the block between the markers; otherwise print each name that is on one side
# only, then `order or text differs` if names agree but the text does not.
doc_matches() {
  local sh="$1" md="$2" gen blk
  gen="$(bash "$sh" --markdown)"
  blk="$(sed -n '/<!-- data-paths:begin/,/<!-- data-paths:end/p' "$md" | sed '1d;$d')"
  [[ -n "$blk" ]] || { echo "INSTALL.md has no data-paths block"; return 1; }
  [[ "$gen" == "$blk" ]] && return 0
  local gn bn n
  gn="$(printf '%s\n' "$gen" | sed -n 's/^- `\([^`]*\)`.*/\1/p' | sed 's,/$,,')"
  bn="$(printf '%s\n' "$blk" | sed -n 's/^- `\([^`]*\)`.*/\1/p' | sed 's,/$,,')"
  for n in $gn; do has_line "$bn" "$n" || echo "data path $n is in data-paths.sh but missing from INSTALL.md"; done
  for n in $bn; do has_line "$gn" "$n" || echo "data path $n is in INSTALL.md but missing from data-paths.sh"; done
  echo "order or text differs"
  return 1
}

out="$(doc_matches "$ROOT/scripts/install-lib/data-paths.sh" "$ROOT/INSTALL.md")" \
  || fail "INSTALL.md's preserved paths do not match data-paths.sh: $out"
pass

# The by-hand removal recipe names every listed path.
recipe="$(sed -n '/^To remove the data by hand/,/^The by-hand commands/p' "$ROOT/INSTALL.md")"
[[ -n "$recipe" ]] || fail "INSTALL.md has no by-hand removal recipe"
for n in $want_names; do
  [[ "$recipe" == *"~/.planar/$n"* ]] || fail "INSTALL.md's by-hand removal recipe omits $n"
done
pass

# Mutation on scratch copies (the real files are never edited).
MUT="$TMP/mut"
mkdir -p "$MUT/scripts/install-lib"
cp "$ROOT"/scripts/install-lib/*.sh "$MUT/scripts/install-lib/"
cp "$ROOT/INSTALL.md" "$MUT/INSTALL.md"
mut_sh="$MUT/scripts/install-lib/data-paths.sh"
grep -q '^workspaces|dir|' "$mut_sh" || fail "mutation target 'workspaces' not found in the table"
sed '/^workspaces|dir|/d' "$mut_sh" > "$mut_sh.new" && mv "$mut_sh.new" "$mut_sh"
if out="$(doc_matches "$mut_sh" "$MUT/INSTALL.md")"; then fail "removing workspaces from data-paths.sh alone went undetected"; fi
[[ "$out" == *'data path workspaces is in INSTALL.md but missing from data-paths.sh'* ]] \
  || fail "the disagreement did not name the entry: $out"
# Removed from INSTALL.md only.
cp "$ROOT/scripts/install-lib/data-paths.sh" "$mut_sh"
sed '/^- `execute\/`/d' "$MUT/INSTALL.md" > "$MUT/INSTALL.new"
if out="$(doc_matches "$mut_sh" "$MUT/INSTALL.new")"; then fail "removing execute from INSTALL.md alone went undetected"; fi
[[ "$out" == *'data path execute is in data-paths.sh but missing from INSTALL.md'* ]] \
  || fail "the disagreement did not name the entry: $out"
# Removed from both: agrees.
sed '/^workspaces|dir|/d' "$mut_sh" > "$mut_sh.new" && mv "$mut_sh.new" "$mut_sh"
sed '/^- `workspaces\/`/d' "$MUT/INSTALL.md" > "$MUT/INSTALL.both"
out="$(doc_matches "$mut_sh" "$MUT/INSTALL.both")" || fail "removal from both sides still disagreed: $out"
# Reordered text only.
cp "$ROOT/scripts/install-lib/data-paths.sh" "$mut_sh"
sed 's/^- `retired\/`.*/&/' "$MUT/INSTALL.md" | awk '/^- `queue-logs\/`/{q=$0;next} /^- `retired\/`/{print; print q; next} {print}' > "$MUT/INSTALL.order"
if out="$(doc_matches "$mut_sh" "$MUT/INSTALL.order")"; then fail "a reordered INSTALL.md went undetected"; fi
pass

# --- driving install.sh and --uninstall ----------------------------------------------

REPO="$TMP/repo"
mkdir -p "$REPO/skills" "$REPO/templates"
cp "$ROOT/install.sh" "$REPO/install.sh"
cp -R "$ROOT/scripts" "$REPO/scripts"
cp -R "$ROOT/agents" "$REPO/agents"
cp -R "$ROOT/skills/planar" "$REPO/skills/planar"
cp "$ROOT/install-cleanup.txt" "$REPO/install-cleanup.txt"
printf 'shipped a\n' > "$REPO/templates/a.toml"
printf 'shipped b\n' > "$REPO/templates/b.toml"
: > "$REPO/CMakeLists.txt"
: > "$REPO/CMakePresets.json"
perl -0pi -e 's#/opt/homebrew/opt/llvm/bin/clang(\+\+)?#/bin/sh#g' "$REPO/install.sh"
# Cleanup entries aimed at every data path, plus one genuine stale entry.
{
  printf 'opt/stale-tool/\n'
  for n in $want_names; do
    case "$n" in queue-logs|retired|workbench|local|workspaces|models|execute|templates) printf '%s/\n' "$n" ;; *) printf '%s\n' "$n" ;; esac
  done
  printf 'workbench/x/README.md\n'
  printf 'templates/a.toml\n'
} >> "$REPO/install-cleanup.txt"

STUBS="$TMP/stubs"
mkdir -p "$STUBS"
cat > "$STUBS/cmake" <<STUB
#!/usr/bin/env bash
# A stub cmake: \`cmake --install D --prefix P\` writes the five fake binaries of
# scripts/fixtures/prebuilt-bundle.sh (stub_binary_write) into P/bin.
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

# run_installer HOME [ENV=VALUE...] -- [install.sh args]: sets RC; output in $TMP/out, $TMP/err.
run_installer() {
  local home="$1"; shift
  local envs=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
  [[ "${1-}" == "--" ]] && shift
  RC=0
  env -u CODEX_HOME -u PLANAR_HOME -u PLANAR_DB -u PLANAR_CONFIG_PATH -u PLANAR_WORKBENCH_ROOT -u PLANAR_LOCAL_HOME \
    -u PLANAR_TEMPLATES_DIR PATH="$STUBS:$PATH" HOME="$home" NO_COLOR=1 ${envs[@]+"${envs[@]}"} \
    "$REPO/install.sh" --build-dir "$home/build" --no-vendor --prefix "$home/.planar" "$@" >"$TMP/out" 2>"$TMP/err" || RC=$?
}

# seed ROOT -- one file in every data path, plus a list file of what was seeded.
seed() {
  local r="$1"
  mkdir -p "$r/queue-logs" "$r/retired/old" "$r/workbench/x" "$r/local/skills/mine" "$r/workspaces" "$r/models" \
           "$r/execute/default" "$r/templates"
  printf 'SQLite format 3\n' > "$r/planar.db"
  printf 'wal bytes\n' > "$r/planar.db-wal"
  printf 'shm bytes\n' > "$r/planar.db-shm"
  printf 'log\n' > "$r/queue-logs/7.log"
  printf 'old\n' > "$r/retired/old/agent.db"
  printf 'readme\n' > "$r/workbench/x/README.md"
  printf '[x]\n' > "$r/config.toml"
  printf 'skill\n' > "$r/local/skills/mine/SKILL.md"
  printf '{}\n' > "$r/workspaces/w.json"
  printf '{}\n' > "$r/models/catalog.json"
  printf 'p\n' > "$r/execute/default/profile.toml"
  printf 'my edited template\n' > "$r/templates/a.toml"
  ( cd "$r" && find planar.db planar.db-wal planar.db-shm queue-logs retired workbench config.toml local workspaces models \
      execute templates -type f | sort ) > "$r/../seeded.list"
}
sums() { ( cd "$1" && while IFS= read -r f; do cksum "$f" 2>/dev/null || echo "MISSING $f"; done < "$2" ); }

H="$TMP/inst/user"
P="$H/.planar"
mkdir -p "$P/opt/stale-tool"
printf 'stale\n' > "$P/opt/stale-tool/f"
seed "$P"
LIST="$H/.planar/../seeded.list"
before="$(sums "$P" "$LIST")"

run_installer "$H" -- 
[[ "$RC" == 0 ]] || fail "install over a seeded prefix failed ($RC): $(cat "$TMP/err")"
[[ "$(sums "$P" "$LIST")" == "$before" ]] || fail "install changed a data path: $(sums "$P" "$LIST" | diff - <(printf '%s\n' "$before") || true)"
[[ ! -e "$P/opt/stale-tool" ]] || fail "the genuine stale cleanup entry was not removed (cleanup is dead)"
[[ -x "$P/bin/planar" ]] || fail "no bin/planar after install"
grep -Fq "kept $P/queue-logs/" "$TMP/out" || fail "install did not say a cleanup entry was skipped for queue-logs: $(cat "$TMP/out")"
grep -Fq "data path 'templates'" "$TMP/out" || fail "install did not name the templates data path"
[[ "$(cat "$P/templates/b.toml")" == "shipped b" ]] || fail "a missing shipped template was not placed"
pass

# --force: templates still missing-only.
rm -f "$P/templates/b.toml"
run_installer "$H" -- --force
[[ "$RC" == 0 ]] || fail "forced install failed ($RC): $(cat "$TMP/err")"
[[ "$(sums "$P" "$LIST")" == "$before" ]] || fail "a forced install changed a data path"
[[ "$(cat "$P/templates/a.toml")" == "my edited template" ]] || fail "--force overwrote an edited template"
[[ "$(cat "$P/templates/b.toml")" == "shipped b" ]] || fail "--force did not place a missing template"
pass

for uforce in "" "--force"; do
  run_installer "$H" -- --uninstall $uforce
  [[ "$RC" == 0 ]] || fail "uninstall $uforce failed ($RC): $(cat "$TMP/err")"
  [[ "$(sums "$P" "$LIST")" == "$before" ]] || fail "uninstall $uforce changed a data path"
  [[ ! -e "$P/bin" && ! -e "$P/scripts" && ! -e "$P/skills" ]] || fail "uninstall $uforce left managed trees: $(ls -A "$P" | tr '\n' ' ')"
  [[ ! -e "$P/install-manifest.json" ]] || fail "uninstall $uforce left the manifest"
  # Reinstall so the second round has something to remove.
  run_installer "$H" --
  [[ "$RC" == 0 ]] || fail "reinstall failed ($RC): $(cat "$TMP/err")"
done
pass

# --- relocated data paths ---------------------------------------------------------------

H2="$TMP/reloc/user"
P2="$H2/.planar"
EXT="$TMP/reloc/ext"
mkdir -p "$P2" "$EXT/wb/p1"
printf 'SQLite format 3\next\n' > "$EXT/p.db"
printf 'ext wal\n' > "$EXT/p.db-wal"
printf 'wb\n' > "$EXT/wb/p1/doc.md"
printf 'SQLite format 3\n' > "$P2/planar.db"
ext_before="$(cd "$EXT" && find . -type f -exec cksum {} + | sort)"
run_installer "$H2" "PLANAR_DB=$EXT/p.db" "PLANAR_WORKBENCH_ROOT=$EXT/wb" --
[[ "$RC" == 0 ]] || fail "install with relocated data failed ($RC): $(cat "$TMP/err")"
grep -Fq "data path planar.db (and its -wal and -shm sidecars) is relocated by PLANAR_DB to $EXT/p.db; it is left where it is" "$TMP/out" \
  || fail "the database relocation was not named: $(cat "$TMP/out")"
grep -Fq "data path workbench is relocated by PLANAR_WORKBENCH_ROOT to $EXT/wb" "$TMP/out" || fail "the workbench relocation was not named"
[[ "$(cd "$EXT" && find . -type f -exec cksum {} + | sort)" == "$ext_before" ]] || fail "install touched relocated files"
run_installer "$H2" "PLANAR_DB=$EXT/p.db" "PLANAR_WORKBENCH_ROOT=$EXT/wb" -- --uninstall
[[ "$RC" == 0 ]] || fail "uninstall with relocated data failed ($RC): $(cat "$TMP/err")"
grep -Fq "data path planar.db (and its -wal and -shm sidecars) is relocated by PLANAR_DB" "$TMP/out" || fail "uninstall did not name the relocation"
[[ "$(cd "$EXT" && find . -type f -exec cksum {} + | sort)" == "$ext_before" ]] || fail "uninstall touched relocated files"
[[ "$(cat "$P2/planar.db")" == "SQLite format 3" ]] || fail "uninstall removed the default-location planar.db"
pass

# A --prefix other than $HOME/.planar: the real default location is named by
# nothing and left alone, and an install there does not call it relocated.
H5="$TMP/alt/user"
mkdir -p "$H5/.planar/workbench/x"
printf 'readme\n' > "$H5/.planar/workbench/x/README.md"
run_installer "$H5" "PLANAR_WORKBENCH_ROOT=$H5/.planar/workbench" -- --prefix "$TMP/alt/inst"
[[ "$RC" == 0 ]] || fail "install with --prefix elsewhere failed ($RC): $(cat "$TMP/err")"
[[ "$(cat "$TMP/out")" != *"relocated by PLANAR_WORKBENCH_ROOT"* ]] || fail "a \$HOME/.planar workbench was reported as relocated: $(cat "$TMP/out")"
[[ "$(cat "$H5/.planar/workbench/x/README.md")" == "readme" ]] || fail "install touched \$HOME/.planar/workbench"
run_installer "$H5" "PLANAR_WORKBENCH_ROOT=$TMP/alt/wb" -- --prefix "$TMP/alt/inst" --uninstall
[[ "$RC" == 0 ]] || fail "uninstall of --prefix elsewhere failed ($RC): $(cat "$TMP/err")"
grep -Fq "data path workbench is relocated by PLANAR_WORKBENCH_ROOT to $TMP/alt/wb" "$TMP/out" || fail "a real relocation was not named: $(cat "$TMP/out")"
[[ "$(cat "$H5/.planar/workbench/x/README.md")" == "readme" ]] || fail "uninstall touched \$HOME/.planar/workbench"
pass

# A data path relocated into a tree the installer refreshes: stop, name it, delete nothing.
H3="$TMP/inside/user"
P3="$H3/.planar"
mkdir -p "$P3/scripts/mytpl" "$P3/bin"
printf 'SQLite format 3\n' > "$P3/planar.db"
printf 'edited\n' > "$P3/scripts/mytpl/t.toml"
run_installer "$H3" "PLANAR_TEMPLATES_DIR=$P3/scripts/mytpl" --
[[ "$RC" != 0 ]] || fail "install removed a tree holding a relocated data path"
grep -Fq "data path 'templates'" "$TMP/err" || fail "the refusal did not name the data path: $(cat "$TMP/err")"
[[ "$(cat "$P3/scripts/mytpl/t.toml")" == "edited" ]] || fail "the template inside the managed tree was removed"
run_installer "$H3" "PLANAR_TEMPLATES_DIR=$P3/scripts/mytpl" -- --uninstall
[[ "$RC" == 0 ]] || fail "uninstall failed ($RC): $(cat "$TMP/err")"
[[ "$(cat "$P3/scripts/mytpl/t.toml")" == "edited" ]] || fail "uninstall removed a data path inside scripts/"
grep -Fq "kept $P3/scripts (data path 'templates')" "$TMP/out" || fail "uninstall did not name what it kept: $(cat "$TMP/out")"
pass

# After an uninstall that leaves only preserved data paths (the database lives
# elsewhere, so no planar.db is in the root), the next install and a second
# uninstall adopt the root without --force: preserved data is ownership
# evidence (tech spec, "The uninstaller").
H4="$TMP/readopt/user"
P4="$H4/.planar"
EXT4="$TMP/readopt/ext"
mkdir -p "$H4" "$EXT4"
printf 'SQLite format 3\next4\n' > "$EXT4/p.db"
run_installer "$H4" "PLANAR_DB=$EXT4/p.db" --
[[ "$RC" == 0 ]] || fail "first install with a relocated database failed ($RC): $(cat "$TMP/err")"
mkdir -p "$P4/workbench/x" "$P4/local/skills/mine"
printf 'readme\n' > "$P4/workbench/x/README.md"
printf '[x]\n' > "$P4/config.toml"
printf 'skill\n' > "$P4/local/skills/mine/SKILL.md"
printf 'notes\n' > "$P4/notes.txt"
run_installer "$H4" "PLANAR_DB=$EXT4/p.db" -- --uninstall
[[ "$RC" == 0 ]] || fail "uninstall (relocated database) failed ($RC): $(cat "$TMP/err")"
[[ ! -e "$P4/planar.db" && -f "$P4/config.toml" && ! -e "$P4/.planar-install" ]] \
  || fail "the fixture is not the intended shape after uninstall: $(ls -A "$P4" | tr '\n' ' ')"
run_installer "$H4" "PLANAR_DB=$EXT4/p.db" --
[[ "$RC" == 0 ]] || fail "reinstall after uninstall was refused without --force ($RC): $(cat "$TMP/err")"
[[ -x "$P4/bin/planar" && "$(cat "$P4/config.toml")" == "[x]" ]] || fail "reinstall after uninstall did not install or touched config.toml"
run_installer "$H4" "PLANAR_DB=$EXT4/p.db" -- --uninstall
[[ "$RC" == 0 ]] || fail "second uninstall was refused without --force ($RC): $(cat "$TMP/err")"
[[ "$(cat "$P4/workbench/x/README.md")" == "readme" && "$(head -1 "$EXT4/p.db")" == "SQLite format 3" ]] \
  || fail "second uninstall touched preserved data"
# Directly after an uninstall the second uninstall is also accepted.
run_installer "$H4" "PLANAR_DB=$EXT4/p.db" -- --uninstall
[[ "$RC" == 0 ]] || fail "a repeated uninstall was refused ($RC): $(cat "$TMP/err")"
pass

# --- the prebuilt path keeps every data path too ---------------------------------------------

# shellcheck source=fixtures/prebuilt-bundle.sh
source "$ROOT/scripts/fixtures/prebuilt-bundle.sh"
PB="$TMP/prebuilt/bundle"
fake_bundle_make "$ROOT" "$PB" "$REPO/install-cleanup.txt"
HP="$TMP/prebuilt/user"
PP="$HP/.planar"
mkdir -p "$PP/opt/stale-tool"
printf 'stale\n' > "$PP/opt/stale-tool/f"
seed "$PP"
printf 'old queue\n' > "$PP/agent.db"
# queue-logs/7.log is an old numbered log: with an agent.db present it moves into
# retired/<date>/ (kept, not removed), so it leaves the unchanged-in-place list.
grep -v '^queue-logs/7.log$' "$PP/../seeded.list" > "$PP/../seeded-prebuilt.list"
PLIST="$PP/../seeded-prebuilt.list"
pbefore="$(sums "$PP" "$PLIST")"
run_prebuilt_install() {
  RC=0
  env -u CODEX_HOME -u PLANAR_HOME -u PLANAR_DB -u PLANAR_CONFIG_PATH -u PLANAR_WORKBENCH_ROOT -u PLANAR_LOCAL_HOME \
    -u PLANAR_TEMPLATES_DIR HOME="$HP" NO_COLOR=1 \
    "$PB/install.sh" --prebuilt "$PB" --no-vendor --prefix "$PP" "$@" >"$TMP/out" 2>"$TMP/err" || RC=$?
}
run_prebuilt_install
[[ "$RC" == 0 ]] || fail "prebuilt install over a seeded prefix failed ($RC): $(cat "$TMP/err")"
[[ "$(sums "$PP" "$PLIST")" == "$pbefore" ]] || fail "prebuilt install changed a data path: $(sums "$PP" "$PLIST" | diff - <(printf '%s\n' "$pbefore") || true)"
[[ ! -e "$PP/opt/stale-tool" ]] || fail "the stale cleanup entry was not removed on the prebuilt path"
[[ -x "$PP/bin/planar" && "$(cat "$PP/templates/a.toml")" == "my edited template" && "$(cat "$PP/templates/b.toml")" == "shipped b" ]] \
  || fail "the prebuilt install did not place the binaries or kept-templates correctly"
[[ ! -e "$PP/agent.db" && "$(cat "$PP/retired/$(date +%Y-%m-%d)/agent.db")" == "old queue" ]] || fail "agent.db was not moved into retired/<date>/"
[[ "$(cat "$PP/retired/$(date +%Y-%m-%d)/queue-logs/7.log")" == "log" ]] || fail "the old numbered log was not kept in retired/<date>/queue-logs/"
[[ "$(cat "$PP/retired/old/agent.db")" == "old" ]] || fail "a file already in retired/ changed"
grep -Fq "kept $PP/queue-logs/" "$TMP/out" || fail "the prebuilt install did not say a cleanup entry was skipped for queue-logs"
run_prebuilt_install --force
[[ "$RC" == 0 ]] || fail "forced prebuilt install failed ($RC): $(cat "$TMP/err")"
[[ "$(sums "$PP" "$PLIST")" == "$pbefore" ]] || fail "a forced prebuilt install changed a data path"
[[ "$(cat "$PP/templates/a.toml")" == "my edited template" ]] || fail "--force overwrote an edited template on the prebuilt path"
pass

printf 'install data paths tests: %s passed\n' "$PASSED"
