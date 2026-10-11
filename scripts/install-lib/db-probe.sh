# shellcheck shell=bash
# shellcheck disable=SC2034  # INSTALL_DB_* and PLANAR_INSTALL_* are read by the caller
#
# db-probe.sh -- the installer's database states (plan 1122, task
# rel-install-order; tech spec 677, "Order of an install", steps 5, 6 and 8;
# decisions 1324-1327).
#
# Sourced by install.sh after queue-retire.sh (it uses _qr_classify_probe and
# _QR_JSON_AWK) and data-paths.sh (_planar_dp_value). Bash 3.2 and base
# utilities only. Functions return a status and never exit; every command
# substitution that may fail clears the ERR trap inside its own subshell.
#
#   planar_db_resolve ROOT
#       PLANAR_INSTALL_DB and PLANAR_INSTALL_CONFIG: the database and config the
#       install probes and initializes. PLANAR_DB / PLANAR_CONFIG_PATH when set
#       (made absolute, as the runtime reads them), else ROOT/planar.db and
#       ROOT/config.toml.
#
#   planar_db_probe BINDIR
#       Classify the database with the binaries in BINDIR, read-only and from /.
#       Sets INSTALL_DB_STATE, INSTALL_DB_VERSION, INSTALL_DB_TARGET and
#       INSTALL_DB_DETAIL (the probe's own diagnostic). States:
#         missing  no file at the path: initialization is required. The probe
#                  never creates one.
#         behind   older than BINDIR's binaries: migration is required.
#         current  the same schema and a usable queue store.
#         ahead    newer than BINDIR's binaries: an older release must never be
#                  installed over it.
#         fault    anything else: unreadable, corrupt, a foreign or incompatible
#                  queue schema at the same version, a non-empty file with no
#                  migration record, or an answer the probe cannot classify.
#       The main-schema boundary is `planar-watch`'s startup handshake (it opens
#       the database read-only and refuses a schema behind or ahead of its own at
#       exit 7, naming both versions); the queue store at the current version is
#       `planar-agent queue status 1 --json`, classified by _qr_classify_probe.
#       Queue compatibility never decides the main schema: an ahead database is
#       ahead whatever its queue tables say.
#
#   planar_db_migrate BINDIR
#       Run BINDIR/planar init --skip-project --allow-no-repo from / against the
#       resolved database and config (it creates a missing database, migrates a
#       behind one and registers no project), then require planar_db_probe to
#       answer current. Status 1, with INSTALL_DB_DETAIL set, otherwise.
#
#   planar_queue_warning BINDIR
#       Name the running and waiting host-queue entries through BINDIR's
#       read-only `planar-watch queue --json`, with the lease warning. Absence or
#       failure prints one line saying the queue could not be checked. Never
#       fails.

planar_db_resolve() {
  local root="${1%/}" v
  v="$(_planar_dp_value PLANAR_DB)"
  PLANAR_INSTALL_DB="${v:-$root/planar.db}"
  v="$(_planar_dp_value PLANAR_CONFIG_PATH)"
  PLANAR_INSTALL_CONFIG="${v:-$root/config.toml}"
}

# _pdb_log / _pdb_warn -- through install.sh's helpers when defined.
_pdb_log() {
  if declare -F log >/dev/null; then log "$@"; else printf '  %s\n' "$*"; fi
}
_pdb_warn() {
  if declare -F warn >/dev/null; then warn "$@"; else printf '  ! %s\n' "$*" >&2; fi
}

# _pdb_run BINDIR BIN ARGS... -- run a binary from / against the resolved
# database and config. Sets _PDB_RC, _PDB_OUT and _PDB_ERR.
_pdb_run() {
  local bindir="$1" bin="$2" errf
  shift 2
  errf="$(trap - ERR; mktemp 2>/dev/null)" || errf=""
  if [ -z "$errf" ]; then
    _PDB_RC=125; _PDB_OUT=""; _PDB_ERR="mktemp failed"; return 0
  fi
  _PDB_RC=0
  _PDB_OUT="$(trap - ERR; cd / && PLANAR_DB="$PLANAR_INSTALL_DB" PLANAR_CONFIG_PATH="$PLANAR_INSTALL_CONFIG" \
    "$bindir/$bin" "$@" 2>"$errf")" || _PDB_RC=$?
  _PDB_ERR="$(cat "$errf" 2>/dev/null || true)"
  rm -f "$errf"
  return 0
}

# _pdb_first_line TEXT -- the first line of TEXT.
_pdb_first_line() {
  printf '%s\n' "$1" | head -n 1
}

planar_db_probe() {
  local bindir="$1" kind first tag size
  INSTALL_DB_STATE="fault"; INSTALL_DB_VERSION=""; INSTALL_DB_TARGET=""; INSTALL_DB_DETAIL=""
  if [ ! -e "$PLANAR_INSTALL_DB" ] && [ ! -L "$PLANAR_INSTALL_DB" ]; then
    INSTALL_DB_STATE="missing"
    INSTALL_DB_DETAIL="no database at $PLANAR_INSTALL_DB"
    return 0
  fi
  _pdb_run "$bindir" planar-watch queue --json
  if [ "$_PDB_RC" = 0 ]; then
    kind="$(trap - ERR; printf '%s\n' "$_PDB_OUT" | tr '\r\n' '  ' | LC_ALL=C awk "$_QR_JSON_AWK")" || kind="bad|0|"
    if [ "${kind%%|*}" != array ]; then
      INSTALL_DB_DETAIL="planar-watch queue --json exited 0 without a JSON list: $(_pdb_first_line "$_PDB_OUT")"
      return 0
    fi
    _pdb_run "$bindir" planar-agent queue status 1 --json
    _qr_classify_probe "$_PDB_RC" "$_PDB_OUT"
    if [ "$QR_VERDICT" = usable ]; then
      INSTALL_DB_STATE="current"
      INSTALL_DB_DETAIL="the schema matches and the queue store is usable ($QR_DETAIL)"
      return 0
    fi
    INSTALL_DB_DETAIL="the queue store is not usable: $QR_VERDICT ($QR_DETAIL)${_PDB_ERR:+: $(_pdb_first_line "$_PDB_ERR")}"
    return 0
  fi
  first="$(_pdb_first_line "$_PDB_ERR")"
  if [ "$_PDB_RC" = 7 ]; then
    INSTALL_DB_VERSION="$(printf '%s\n' "$first" | sed -n 's/^error: schema version \([0-9][0-9]*\) in .*$/\1/p')"
    case "$_PDB_ERR" in
      *"error: SchemaVersionAhead"*)
        tag=ahead
        INSTALL_DB_TARGET="$(printf '%s\n' "$first" | sed -n 's/^.* newer than this binary.s embedded max (\([0-9][0-9]*\)).*$/\1/p')"
        ;;
      *"error: SchemaVersionBehind"*)
        tag=behind
        INSTALL_DB_TARGET="$(printf '%s\n' "$first" | sed -n 's/^.* older than this binary.s minimum of \([0-9][0-9]*\).*$/\1/p')"
        ;;
      *) tag="" ;;
    esac
    if [ -n "$tag" ] && [ -n "$INSTALL_DB_VERSION" ] && [ -n "$INSTALL_DB_TARGET" ]; then
      if [ "$tag" = behind ] && [ "$INSTALL_DB_VERSION" = 0 ]; then
        size="$(trap - ERR; wc -c < "$PLANAR_INSTALL_DB" 2>/dev/null | tr -d ' ')" || size=""
        if [ "$size" != 0 ]; then
          INSTALL_DB_DETAIL="$PLANAR_INSTALL_DB is not empty but records no applied migration, so it is not a Planar database this installer can migrate: $first"
          return 0
        fi
      fi
      INSTALL_DB_STATE="$tag"
      INSTALL_DB_DETAIL="$first"
      return 0
    fi
  fi
  INSTALL_DB_DETAIL="planar-watch exited $_PDB_RC: ${first:-$(_pdb_first_line "$_PDB_OUT")}"
  return 0
}

planar_db_migrate() {
  local bindir="$1"
  _pdb_run "$bindir" planar init --skip-project --allow-no-repo
  if [ "$_PDB_RC" != 0 ]; then
    INSTALL_DB_DETAIL="planar init exited $_PDB_RC: ${_PDB_ERR:-$_PDB_OUT}"
    return 1
  fi
  [ -z "$_PDB_OUT" ] || _pdb_log "$_PDB_OUT"
  planar_db_probe "$bindir"
  if [ "$INSTALL_DB_STATE" != current ]; then
    INSTALL_DB_DETAIL="after planar init the database is $INSTALL_DB_STATE: $INSTALL_DB_DETAIL"
    return 1
  fi
  return 0
}

planar_queue_warning() {
  local bindir="$1" kind entries line seq state list="" n=0
  if [ ! -x "$bindir/planar-watch" ]; then
    _pdb_warn "the host queue could not be checked for running or waiting entries: $bindir/planar-watch is not installed (skipped)"
    return 0
  fi
  if [ ! -e "$PLANAR_INSTALL_DB" ]; then
    _pdb_log "no database yet, so no host-queue entries to check"
    return 0
  fi
  _pdb_run "$bindir" planar-watch queue --json
  kind="$(trap - ERR; printf '%s\n' "$_PDB_OUT" | tr '\r\n' '  ' | LC_ALL=C awk "$_QR_JSON_AWK")" || kind="bad|0|"
  if [ "$_PDB_RC" != 0 ] || [ "${kind%%|*}" != array ]; then
    _pdb_warn "the host queue could not be checked for running or waiting entries (the installed planar-watch exited $_PDB_RC: $(_pdb_first_line "${_PDB_ERR:-$_PDB_OUT}")); skipped"
    return 0
  fi
  entries="$(trap - ERR; printf '%s' "$_PDB_OUT" | grep -oE '\{"seq":[0-9]+,"state":"(running|waiting)"' 2>/dev/null)" || entries=""
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    seq="$(printf '%s' "$line" | sed -n 's/^{"seq":\([0-9]*\),.*$/\1/p')"
    state="$(printf '%s' "$line" | sed -n 's/^.*"state":"\([a-z]*\)"$/\1/p')"
    list="$list${list:+, }$seq ($state)"
    n=$((n + 1))
  done <<EOF
$entries
EOF
  if [ "$n" -eq 0 ]; then
    _pdb_log "host queue: no running or waiting entries"
    return 0
  fi
  _pdb_warn "the host queue has $n running or waiting entr$([ "$n" -eq 1 ] && echo y || echo ies): $list. A command already running keeps its old binary; a claim lease that \`planar-agent queue run --claim\` renews can lapse while the database is migrated. The install continues."
  return 0
}
