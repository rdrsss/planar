# shellcheck shell=bash
# shellcheck disable=SC2034,SC2016  # INSTALL_DB_* are read by the caller; the awk program is literal
#
# db-backup.sh -- the installer's copy of planar.db before a migration that
# drops tables (tech spec 693, component 12 "Deletion", and the decision "Only
# routing tables are dropped, after a backup").
#
# Sourced by install.sh after db-probe.sh (it uses planar_db_probe and
# PLANAR_INSTALL_DB) and install-state.sh (planar_install_fault, when defined).
# Bash 3.2 and base utilities only. Functions return a status and never exit.
#
#   planar_migrations_dropping_tables DIR FROM TO
#       Print one line per migration in DIR numbered above FROM and at or below
#       TO whose up file drops a table: `NNNNN_name.up.sql: table table ...`.
#       A table counts as dropped when the file has `drop table [if exists]
#       NAME` and neither creates NAME (`create table [if not exists] NAME`)
#       nor renames another table to it (`rename to NAME`), so a table rebuild
#       (create the new table, copy, drop the old one, rename) is not a drop.
#       A `temp.` table is never counted, and `--` comments are ignored. Status
#       1 when DIR is not a directory.
#
#   planar_db_backup BINDIR ROOT VERSION
#       Copy the resolved database (PLANAR_INSTALL_DB) with its -wal and -shm
#       files, when present, into a new directory ROOT/backups/<UTC
#       time>-schema-VERSION/, keeping their base names so the copy opens as
#       it stands. Each copy is compared with its source afterwards; a
#       difference (a writer committed during the copy) copies again, up to
#       three attempts. The copy is then opened with BINDIR's read-only
#       planar-watch, which must find it at schema VERSION. Sets
#       INSTALL_DB_BACKUP to the copied database's path. On failure removes
#       the directory it created, sets INSTALL_DB_DETAIL and returns 1. The
#       caller holds the mutation lock; nothing here migrates or changes the
#       source database.

# Reads one up migration on stdin; prints the tables it drops for good, space
# separated. One string of the whole file, comments removed, so a statement
# split across lines still matches.
_PDB_DROPS_AWK='
{ line = tolower($0); sub(/--.*/, "", line); text = text " " line }
END {
  gsub(/[ \t\r]+/, " ", text)
  s = text
  while (match(s, /drop table (if exists )?[a-z0-9_."`]+/)) {
    n = substr(s, RSTART, RLENGTH); s = substr(s, RSTART + RLENGTH)
    sub(/^drop table (if exists )?/, "", n); gsub(/["`]/, "", n)
    if (n ~ /^temp\./) continue
    sub(/^main\./, "", n)
    dropped[n] = 1
  }
  s = text
  while (match(s, /create table (if not exists )?[a-z0-9_."`]+/)) {
    n = substr(s, RSTART, RLENGTH); s = substr(s, RSTART + RLENGTH)
    sub(/^create table (if not exists )?/, "", n); gsub(/["`]/, "", n); sub(/^main\./, "", n)
    made[n] = 1
  }
  s = text
  while (match(s, /rename to [a-z0-9_."`]+/)) {
    n = substr(s, RSTART, RLENGTH); s = substr(s, RSTART + RLENGTH)
    sub(/^rename to /, "", n); gsub(/["`]/, "", n); sub(/^main\./, "", n)
    made[n] = 1
  }
  out = ""
  for (n in dropped) if (!(n in made)) out = out (out == "" ? "" : " ") n
  print out
}'

planar_migrations_dropping_tables() {
  local dir="$1" from="$2" to="$3" f base n tables
  [ -d "$dir" ] || return 1
  for f in "$dir"/*.up.sql; do
    [ -f "$f" ] || continue
    base="${f##*/}"
    n="${base%%_*}"
    case "$n" in ''|*[!0-9]*) continue ;; esac
    n=$((10#$n))
    [ "$n" -gt "$from" ] && [ "$n" -le "$to" ] || continue
    tables="$(trap - ERR; LC_ALL=C awk "$_PDB_DROPS_AWK" < "$f" | tr ' ' '\n' | LC_ALL=C sort | tr '\n' ' ')" || tables=""
    tables="${tables% }"
    [ -z "$tables" ] || printf '%s: %s\n' "$base" "$tables"
  done
  return 0
}

# _pdb_backup_fail DIR DETAIL -- remove the partial backup directory DIR, set
# INSTALL_DB_DETAIL, status 1.
_pdb_backup_fail() {
  [ -z "$1" ] || rm -rf "$1"
  INSTALL_DB_DETAIL="$2"
  return 1
}

planar_db_backup() {
  local bindir="$1" root="${2%/}" version="$3" dir name stamp i=1 attempt base src same s
  local saved_db saved_state saved_version saved_target saved_detail
  INSTALL_DB_BACKUP=""
  src="$PLANAR_INSTALL_DB"
  base="${src##*/}"
  [ -f "$src" ] || { INSTALL_DB_DETAIL="$src is not a regular file"; return 1; }
  if [ ! -d "$root/backups" ]; then
    (trap - ERR; mkdir -m 700 "$root/backups" 2>/dev/null) \
      || { INSTALL_DB_DETAIL="cannot create the directory $root/backups"; return 1; }
  fi
  stamp="$(trap - ERR; date -u +%Y%m%dT%H%M%SZ 2>/dev/null)" || stamp=""
  [ -n "$stamp" ] || { INSTALL_DB_DETAIL="date could not name the backup"; return 1; }
  name="$stamp-schema-$version"
  dir="$root/backups/$name"
  while ! (trap - ERR; mkdir -m 700 "$dir" 2>/dev/null); do
    [ -d "$root/backups" ] && [ -w "$root/backups" ] \
      || { INSTALL_DB_DETAIL="cannot create a directory in $root/backups"; return 1; }
    i=$((i + 1))
    [ "$i" -le 100 ] || { INSTALL_DB_DETAIL="no free backup name for $name in $root/backups"; return 1; }
    dir="$root/backups/$name-$i"
  done
  attempt=1
  while :; do
    same=1
    for s in "" -wal -shm; do
      rm -f "$dir/$base$s"
      if [ -e "$src$s" ]; then
        (trap - ERR; cp -p "$src$s" "$dir/$base$s" 2>/dev/null) \
          || { _pdb_backup_fail "$dir" "cannot copy $src$s into $dir"; return 1; }
      fi
    done
    for s in "" -wal -shm; do
      if [ -e "$src$s" ] || [ -e "$dir/$base$s" ]; then
        (trap - ERR; cmp -s "$src$s" "$dir/$base$s") || same=0
      fi
    done
    [ "$same" = 1 ] && break
    attempt=$((attempt + 1))
    [ "$attempt" -le 3 ] \
      || { _pdb_backup_fail "$dir" "$src kept changing while it was copied (three attempts); stop whatever is writing to it and re-run"; return 1; }
  done
  if declare -F planar_install_fault >/dev/null; then
    planar_install_fault backup || { _pdb_backup_fail "$dir" "test fault injected into the backup"; return 1; }
  fi
  # The copy must open, read-only, at the schema the source is at.
  saved_db="$PLANAR_INSTALL_DB"; saved_state="${INSTALL_DB_STATE-}"; saved_version="${INSTALL_DB_VERSION-}"
  saved_target="${INSTALL_DB_TARGET-}"; saved_detail="${INSTALL_DB_DETAIL-}"
  PLANAR_INSTALL_DB="$dir/$base"
  planar_db_probe "$bindir"
  PLANAR_INSTALL_DB="$saved_db"
  if [ "$INSTALL_DB_STATE" != behind ] || [ "$INSTALL_DB_VERSION" != "$version" ]; then
    s="the copy $dir/$base does not open at schema $version: ${INSTALL_DB_STATE:-unknown} (${INSTALL_DB_DETAIL})"
    INSTALL_DB_STATE="$saved_state"; INSTALL_DB_VERSION="$saved_version"; INSTALL_DB_TARGET="$saved_target"
    _pdb_backup_fail "$dir" "$s"
    return 1
  fi
  INSTALL_DB_STATE="$saved_state"; INSTALL_DB_VERSION="$saved_version"; INSTALL_DB_TARGET="$saved_target"
  INSTALL_DB_DETAIL="$saved_detail"
  INSTALL_DB_BACKUP="$dir/$base"
  return 0
}
