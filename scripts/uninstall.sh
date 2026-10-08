#!/usr/bin/env bash
#
# uninstall.sh -- remove a Planar installation (plan 1122, task
# rel-uninstall-script; tech spec 677, "The uninstaller" and "Mutation
# ownership and cleanup"; decisions 1328-1331 and 1333).
#
# Installed as ~/.planar/bin/planar-uninstall, shipped at the root of every
# release bundle as uninstall.sh, and run from a checkout as
# scripts/uninstall.sh; `install.sh --uninstall` is a call into it. It is a
# standalone bash script, so it works when the Planar binaries are broken,
# schema-locked or already gone, and it needs no python3: stock macOS
# /bin/bash 3.2 and base utilities only.
#
# Usage:
#   planar-uninstall [--prefix DIR] [--purge] [--yes]
#
# What it does, in order, under the common mutation lock (mutation-lock.sh):
#   1. Applies the prefix guard (prefix-guard.sh): $HOME, / and the empty
#      string are refused at exit 2; a root with no Planar sign at exit 1.
#   2. Takes the mutation lock of the root, refusing a competing install,
#      update or uninstall before anything is removed. The lock directory
#      `<root>.lock` lives outside the root and is never removed, --purge
#      included.
#   3. Records `phase=uninstalling` in the recovery journal (journal.sh), which
#      ends any pending install for good: no later install replays it. The
#      journal keeps the interrupted install's staging and backup ownership.
#   4. Removes every vendor path the version 2 install-manifest.json records,
#      each only while it is still what Planar placed (ownership.sh). The
#      manifest is read with sed; a line it cannot parse is reported by number
#      and its target left in place. With no manifest, or a version 1 one,
#      nothing under the vendor directories is removed.
#   5. Removes the journal-owned install staging and `<subtree>.old` backups,
#      an abandoned updater's recorded temporary directory, the managed
#      subtrees (never following a symlink), the retired paths named in
#      install-cleanup.txt, and install-cleanup.txt, release.json, the install
#      stamp and the manifest.
#   6. Keeps and names every data path (data-paths.sh) and the legacy agent.db.
#      With --purge it names and removes them instead, except a relocated one,
#      which is named and left where it is.
#   7. Keeps and reports every other entry it does not know.
#   8. Writes the uninstalled marker (journal.sh) when the root is kept, so the
#      next install adopts it without --force, removes the journal last, and
#      removes the root when nothing is left in it.
#   9. Offers to remove Planar binaries left in ~/.local/bin by the retired
#      `make install`; removes them with --yes. With no terminal on stdin it
#      asks nothing and leaves them.
#
# An interrupted uninstall is finished by running it again: the uninstalling
# journal makes the next run finish the removals, and no install resumes the
# cancelled one. The durable retry command is printed before the first removal.
#
# Exit status: 0 done (kept entries are reported, not errors); 1 refused (a
# competing owner, a recovery journal it cannot validate, a root that is not
# Planar's) or failed; 2 a dangerous root, or --force; 64 a usage error.

set -u
set -o pipefail

usage() {
  cat <<'EOF'
planar-uninstall -- remove a Planar installation.

Usage:
  planar-uninstall [--prefix DIR] [--purge] [--yes]

Options:
  --prefix DIR   Install root (default: $PLANAR_HOME, else ~/.planar)
  --purge        Also remove the data paths (planar.db, workbench/, config.toml,
                 templates/, ...) and the legacy agent.db, then the install root
                 when it is empty. A data path relocated by PLANAR_DB,
                 PLANAR_CONFIG_PATH, PLANAR_WORKBENCH_ROOT, PLANAR_LOCAL_HOME or
                 PLANAR_TEMPLATES_DIR is named and left where it is.
  --yes, -y      Remove Planar binaries found in ~/.local/bin without asking.
  -h, --help     Show this help and exit.

Without --purge every data path is kept and named. Entries Planar does not
know are kept and reported. There is no --force: an uninstall never removes
what it cannot prove is Planar's.
EOF
}

# ---------- arguments ----------

# `${VAR-default}`, not `:-`: PLANAR_HOME set to the empty string must reach the
# prefix guard and be refused.
PLANAR_HOME="${PLANAR_HOME-$HOME/.planar}"
PURGE=0
YES=0
FORCE_GIVEN=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --prefix)
      if [ "$#" -lt 2 ]; then printf 'planar-uninstall: --prefix needs a directory\n' >&2; exit 64; fi
      PLANAR_HOME="$2"; shift 2 ;;
    --purge) PURGE=1; shift ;;
    --yes|-y) YES=1; shift ;;
    --force) FORCE_GIVEN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'planar-uninstall: unknown option: %s\n\n' "$1" >&2; usage >&2; exit 64 ;;
  esac
done
if [ "$FORCE_GIVEN" -eq 1 ]; then
  printf 'planar-uninstall: --force is not an uninstall option: an uninstall never removes what it cannot prove is Planar'"'"'s, and the old --uninstall --force no longer exists. To remove the data paths too (planar.db, workbench/, templates/, ...), pass --purge. Nothing was changed.\n' >&2
  exit 2
fi

# ---------- the shared install library ----------

# The checkout runs scripts/uninstall.sh (scripts/install-lib beside it), a
# bundle runs uninstall.sh at its root (scripts/install-lib below it) and an
# install runs bin/planar-uninstall (scripts/install-lib beside bin/). The
# installed copy reads the library before it removes anything, so removing
# scripts/ later does not matter.
SELF="${BASH_SOURCE[0]}"
SELF_DIR="$(cd "$(dirname "$SELF")" && pwd -P)"
SELF_PATH="$SELF_DIR/$(basename "$SELF")"
LIB=""
for _c in "$SELF_DIR/install-lib" "$SELF_DIR/scripts/install-lib" "$SELF_DIR/../scripts/install-lib"; do
  if [ -f "$_c/prefix-guard.sh" ] && [ -f "$_c/ownership.sh" ]; then LIB="$(cd "$_c" && pwd -P)"; break; fi
done
if [ -z "$LIB" ]; then
  printf 'planar-uninstall: cannot find the installer library (scripts/install-lib) beside %s, so nothing was changed. Run uninstall.sh from a Planar release bundle or a checkout'"'"'s scripts/uninstall.sh.\n' "$SELF_PATH" >&2
  exit 1
fi
# shellcheck source=scripts/install-lib/prefix-guard.sh
source "$LIB/prefix-guard.sh"
# shellcheck source=scripts/install-lib/mutation-lock.sh
source "$LIB/mutation-lock.sh"
# shellcheck source=scripts/install-lib/install-state.sh
source "$LIB/install-state.sh"
# shellcheck source=scripts/install-lib/managed-lists.sh
source "$LIB/managed-lists.sh"
# shellcheck source=scripts/install-lib/ownership.sh
source "$LIB/ownership.sh"
# shellcheck source=scripts/install-lib/release.sh
source "$LIB/release.sh"

# The managed subtrees (PLANAR_JOURNAL_SUBTREES) come from managed-lists.sh, the
# one copy install.sh sources too.
PLANAR_BINARY_NAMES="planar planar-agent planar-watch planar-execute planar-ext"

# ---------- output ----------

WARN_COUNT=0
KEPT_UNKNOWN=0
title() { printf '\n==> %s\n' "$*"; }
log()   { printf '  %s\n' "$*"; }
warn()  { WARN_COUNT=$((WARN_COUNT + 1)); printf '  ! %s\n' "$*" >&2; }
die()   { printf '\nplanar-uninstall: %s\n' "$*" >&2; exit 1; }

# ---------- the prefix guard ----------

_guard_rc=0
planar_prefix_guard "$PLANAR_HOME" 0 uninstall || _guard_rc=$?
[ "$_guard_rc" -eq 0 ] || exit "$_guard_rc"
ROOT_C="$PLANAR_PREFIX_CANON"
case "$ROOT_C" in *"
"*|*"	"*) printf 'planar-uninstall: the install root %s holds a newline or tab; refusing\n' "$ROOT_C" >&2; exit 2 ;; esac
HOME_PLANAR_C="$(planar_canonical_path "$HOME/.planar" 2>/dev/null || printf '%s' "$HOME/.planar")"
# ownership.sh judges symlinks against PLANAR_HOME; the canonical spelling is the
# one the lock, the journal and every removal use.
MANIFEST="$ROOT_C/install-manifest.json"

# ---------- the durable retry command ----------

# uninstall_args -- this run's options, as the retry must repeat them.
uninstall_args() {
  local out=""
  [ "$ROOT_C" = "$HOME_PLANAR_C" ] || out="$out --prefix $(printf '%q' "$ROOT_C")"
  [ "$PURGE" -eq 0 ] || out="$out --purge"
  [ "$YES" -eq 0 ] || out="$out --yes"
  printf '%s' "$out"
}

# retry_command -- the command that finishes this uninstall after an
# interruption, which must not depend on the installed copy (this run removes
# it): a release install names that release's bundled uninstaller, downloaded
# afresh and verified against SHA256SUMS before it is extracted; otherwise the
# source checkout (the one this script runs from, or the one the install
# recorded in release.json when this is the installed copy), or the bundle
# it runs from when that lies outside the root.
retry_command() {
  local args v os arch base checkout pr=""
  args="$(uninstall_args)"
  if [ -f "$ROOT_C/release.json" ]; then
    # A source install records its checkout; a usable one wins over the release
    # branch below, because a source build of a tagged commit carries the tag
    # as its version and has no release asset to download. A bundle's
    # release.json never has the key.
    checkout="$(release_source_checkout "$ROOT_C/release.json")"
    if [[ "$checkout" == /* ]] && [ -f "$checkout/install.sh" ] && [ -f "$checkout/scripts/uninstall.sh" ]; then
      printf 'cd %s && ./install.sh --uninstall%s' "$(printf '%q' "$checkout")" "$args"
      return 0
    fi
    v="$(release_field "$ROOT_C/release.json" version)"
    os="$(release_field "$ROOT_C/release.json" os)"
    arch="$(release_field "$ROOT_C/release.json" arch)"
    if [[ "$v" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ && "$os" =~ ^[a-z0-9_]+$ && "$arch" =~ ^[a-z0-9_]+$ ]]; then
      base="$DEFAULT_RELEASE_BASE"
      if [ -n "${PLANAR_RELEASE_URL-}" ] && release_base_valid "$PLANAR_RELEASE_URL"; then base="${PLANAR_RELEASE_URL%/}"; fi
      # As the bootstrap does, an https base never follows a redirect to http.
      case "$base" in https://*) pr=" --proto-redir =https" ;; esac
      # The retry downloads SHA256SUMS and the archive, verifies the archive
      # against its one record (sha256sum -c or shasum -a 256 -c, as the
      # bootstrap does) and only then extracts. A mismatch, a missing or
      # duplicate record, or no checksum tool refuses naming the asset and
      # extracts nothing. It runs in a subshell so it leaves no variable
      # behind, and removes its scratch directory however it ends.
      # shellcheck disable=SC2016  # the command is printed for the operator, not run
      printf '(d="$(mktemp -d "${TMPDIR:-/tmp}/planar-uninstall.XXXXXX")" || exit 1; trap \047rm -rf "$d"\047 EXIT; a=planar-%s-%s.tar.gz && u=%s && curl -fsSL%s "$u/SHA256SUMS" -o "$d/SHA256SUMS" && curl -fsSL%s "$u/$a" -o "$d/$a" && { (cd "$d" && [ "$(grep -cx "[0-9a-f]\\{64\\}  $a" SHA256SUMS)" = 1 ] && grep -x "[0-9a-f]\\{64\\}  $a" SHA256SUMS > asset.sha256 && { command -v sha256sum >/dev/null 2>&1 || command -v shasum >/dev/null 2>&1 || { echo "planar-uninstall: no sha256sum or shasum to verify $a with; nothing was extracted" >&2; exit 2; }; { sha256sum -c asset.sha256 || shasum -a 256 -c asset.sha256; } >/dev/null 2>&1; }) || { [ "$?" = 2 ] || echo "planar-uninstall: checksum verification failed for $a: it does not match SHA256SUMS; nothing was extracted" >&2; exit 1; }; } && tar -xzf "$d/$a" -C "$d" && bash "$d/planar-%s-%s/uninstall.sh"%s)' \
        "$os" "$arch" "$(printf '%q' "$base/download/$v")" "$pr" "$pr" "$os" "$arch" "$args"
      return 0
    fi
  fi
  case "$SELF_DIR/" in
    "$ROOT_C/"*)
      ;;
    *)
      checkout="$(cd "$SELF_DIR/.." && pwd -P)"
      if [ "$(basename "$SELF_DIR")" = scripts ] && [ -f "$checkout/install.sh" ] && [ -f "$checkout/CMakeLists.txt" ]; then
        printf 'cd %s && ./install.sh --uninstall%s' "$(printf '%q' "$checkout")" "$args"
      else
        printf 'bash %s%s' "$(printf '%q' "$SELF_PATH")" "$args"
      fi
      return 0
      ;;
  esac
  printf 're-run uninstall.sh from a Planar release bundle or a checkout'"'"'s scripts/uninstall.sh with:%s' "${args:- (no options)}"
}

# ---------- removal helpers ----------

# remove_path PATH WHAT -- remove PATH without following a symlink, and say so.
remove_path() {
  if [ -L "$1" ]; then rm -f "$1"; else rm -rf "$1"; fi
  if [ -e "$1" ] || [ -L "$1" ]; then
    warn "could not remove $2 $1"
    return 1
  fi
  log "removed $2 $1"
}

# in_root_removable PATH -- PATH may be removed by an ordinary uninstall: it is
# under the root and is not, and holds no, data path. Names a blocking data path.
in_root_removable() {
  case "$1" in "$ROOT_C"/*) ;; *) return 1 ;; esac
  if planar_removal_blocked "$ROOT_C" "$1"; then
    log "kept $1 (data path '$PLANAR_DATA_PATH_HIT')"
    return 1
  fi
  return 0
}

# ---------- the manifest's projections ----------

# The version 2 writer (install-manifest.sh) emits each projection as one line:
#   {"vendor": S, "kind": S, "name": S, "staged_path": S, "installed_path": S,
#    "install_kind": S, "source_digest": S, "projection_digest": S}
# indented four spaces, with a comma after every row but the last, between the
# lines `  "projections": [` and `  ]`. S is a JSON string; the paths need only
# the escapes \\ and \" (a value holding a control character would carry
# another escape, and such a line is reported, not guessed at).
TAB="	"
_S='"(([^"\\[:cntrl:]]|\\["\\])*)"'
_PROJ_RE="^    [{]\"vendor\": $_S, \"kind\": $_S, \"name\": $_S, \"staged_path\": $_S, \"installed_path\": $_S, \"install_kind\": $_S, \"source_digest\": $_S, \"projection_digest\": ${_S}[}],?\$"

# json_unescape RAW -- undo the writer's \\ and \" escapes.
json_unescape() {
  # shellcheck disable=SC1003  # bs is one backslash
  local v="$1" soh=$'\001' bs='\' q='"'
  v="${v//"$bs$bs"/$soh}"
  v="${v//"$bs$q"/$q}"
  v="${v//$soh/$bs}"
  printf '%s' "$v"
}

# manifest_version FILE -- print the manifest's version number, or nothing.
manifest_version() {
  sed -n -E 's/^  "version": ([0-9]+),$/\1/p' "$1" 2>/dev/null | head -n 1
}

# manifest_projections FILE -- for every line of the projections array print
# `<line number>` and then either `P<TAB>staged<TAB>installed<TAB>vendor<TAB>kind`
# (fields still escaped) or `B` for a line that does not parse.
manifest_projections() {
  sed -n -E \
    -e '/^  "projections": \[$/,/^  \]$/{' \
    -e '/^  "projections": \[$/d' \
    -e '/^  \]$/d' \
    -e '/^$/d' \
    -e "s/$_PROJ_RE/P$TAB\\7$TAB\\9$TAB\\1$TAB\\3/" \
    -e 't ok' \
    -e 's/.*/B/' \
    -e ':ok' \
    -e '=' \
    -e 'p' \
    -e '}' "$1"
}

# staged_under_root PATH -- PATH's parent resolves inside the root (the staged
# trees live there; a link-mode staged entry is itself a symlink, so only its
# parent is resolved).
staged_under_root() {
  local parent
  parent="$(planar_canonical_path "$(dirname "$1")" 2>/dev/null)" || return 1
  case "$parent/" in "$ROOT_C"/*) return 0 ;; esac
  return 1
}

VENDOR_REMOVED=0
VENDOR_LEFT=0
# remove_projections -- remove each recorded projection that is still Planar's.
remove_projections() {
  local ver lineno rec rest staged installed vendor kind
  if [ ! -f "$MANIFEST" ] || [ -L "$MANIFEST" ]; then
    log "no install-manifest.json: no vendor projections are recorded, so nothing under the vendor directories was removed"
    return 0
  fi
  ver="$(manifest_version "$MANIFEST")"
  if [ "$ver" != 2 ]; then
    if [ -z "$ver" ]; then
      warn "$MANIFEST has no readable version; it is treated as no manifest"
    fi
    log "install-manifest.json is version ${ver:-unknown}, which records no projections this uninstaller reads, so nothing under the vendor directories was removed"
    return 0
  fi
  while IFS= read -r lineno && IFS= read -r rec; do
    if [ "${rec%%"$TAB"*}" != P ]; then
      warn "install-manifest.json line $lineno: cannot read this projection; its target is left in place"
      VENDOR_LEFT=$((VENDOR_LEFT + 1))
      continue
    fi
    # Split on the tabs by hand: a tab IFS would merge an empty field.
    rest="${rec#*"$TAB"}"
    staged="$(json_unescape "${rest%%"$TAB"*}")"; rest="${rest#*"$TAB"}"
    installed="$(json_unescape "${rest%%"$TAB"*}")"; rest="${rest#*"$TAB"}"
    vendor="$(json_unescape "${rest%%"$TAB"*}")"; rest="${rest#*"$TAB"}"
    kind="$(json_unescape "$rest")"
    case "$installed" in
      /*) ;;
      *) warn "install-manifest.json line $lineno: the installed path '$installed' is not absolute; left in place"; VENDOR_LEFT=$((VENDOR_LEFT + 1)); continue ;;
    esac
    case "/$installed/" in
      */../*|*/./*) warn "install-manifest.json line $lineno: the installed path $installed is not a plain path; left in place"; VENDOR_LEFT=$((VENDOR_LEFT + 1)); continue ;;
    esac
    [ -e "$installed" ] || [ -L "$installed" ] || continue
    if ! staged_under_root "$staged"; then
      warn "left $installed: its recorded source $staged is not inside $ROOT_C, so its ownership cannot be proven"
      VENDOR_LEFT=$((VENDOR_LEFT + 1)); continue
    fi
    if planar_removal_blocked "$ROOT_C" "$installed"; then
      warn "left $installed: it is or holds the data path '$PLANAR_DATA_PATH_HIT'"
      VENDOR_LEFT=$((VENDOR_LEFT + 1)); continue
    fi
    if uninstall_owned "$staged" "$installed" "$vendor" "$kind"; then
      remove_path "$installed" "recorded $vendor $kind" && VENDOR_REMOVED=$((VENDOR_REMOVED + 1))
      planar_install_fault uninstall-projection || exit 1
    else
      warn "left $installed: it is not what Planar placed there (changed after the install?); remove it yourself if you do not need it"
      VENDOR_LEFT=$((VENDOR_LEFT + 1))
    fi
  done <<EOF_ROWS
$(manifest_projections "$MANIFEST")
EOF_ROWS
  log "removed $VENDOR_REMOVED recorded vendor path(s); left $VENDOR_LEFT"
  report_vendor_leftovers
}

# report_vendor_leftovers -- name every entry still under a planar name in the
# vendor directories Planar places into. Read only: nothing here is removed.
report_vendor_leftovers() {
  local d e
  for d in "$HOME/.claude/skills" "$HOME/.agents/skills" "$HOME/.gemini/antigravity-cli/skills" \
           "$HOME/.claude/agents" "${CODEX_HOME:-$HOME/.codex}/agents" "$HOME/.copilot/agents" "$HOME/.gemini/agents" \
           "$HOME/.gemini/antigravity-cli/agents" "$HOME/.config/opencode/agents"; do
    [ -d "$d" ] || continue
    for e in "$d"/planar "$d"/planar-*; do
      [ -e "$e" ] || [ -L "$e" ] || continue
      warn "left $e: Planar did not place it as recorded"
    done
  done
}

# ---------- owned recovery resources ----------

# remove_recovery -- the install staging and backups the journal owns, and an
# abandoned updater's recorded temporary directory. Unknown ones stay.
remove_recovery() {
  local s n sk rl o oi
  for s in ${J_staging-}; do
    case "$s" in .staging-*) ;; *) continue ;; esac
    case "$s" in */*|*[!A-Za-z0-9._-]*) continue ;; esac
    if [ -e "$ROOT_C/$s" ] || [ -L "$ROOT_C/$s" ]; then
      in_root_removable "$ROOT_C/$s" && remove_path "$ROOT_C/$s" "the interrupted install's staging"
    fi
  done
  planar_state_dispose_staging "$ROOT_C"
  for n in $PLANAR_JOURNAL_SUBTREES; do
    sk="$(planar_journal_sub_key "$n")"
    rl="$(_ps_get "${sk}_live")"
    [ -n "$rl" ] && [ "$rl" != none ] || continue
    o="$ROOT_C/$n.old"
    oi="$(_ps_ino "$o")"
    [ "$oi" != none ] || continue
    if [ "$oi" = "$rl" ]; then
      in_root_removable "$o" && remove_path "$o" "the interrupted install's backup"
    fi
  done
  planar_journal_write "$ROOT_C" || warn "could not update $ROOT_C/.planar-journal after removing the recovery resources"
  if [ -n "$PLANAR_LOCK_RECLAIMED_TMP" ]; then
    remove_path "$PLANAR_LOCK_RECLAIMED_TMP" "the abandoned updater's temporary directory"
  fi
  if [ -d "$ROOT_C/$PLANAR_LOCK_UPDATE_NS" ] && [ ! -L "$ROOT_C/$PLANAR_LOCK_UPDATE_NS" ]; then
    rmdir "$ROOT_C/$PLANAR_LOCK_UPDATE_NS" 2>/dev/null || true
  fi
}

# ---------- managed subtrees, retired paths, install records ----------

remove_managed() {
  local n
  # bin/ first: it holds this uninstaller's installed copy, so a run killed
  # later never leaves planar-uninstall without the library it reads.
  for n in $PLANAR_JOURNAL_SUBTREES; do
    if [ -e "$ROOT_C/$n" ] || [ -L "$ROOT_C/$n" ]; then
      in_root_removable "$ROOT_C/$n" && remove_path "$ROOT_C/$n" "managed subtree"
      planar_install_fault "uninstall-subtree:$n" || exit 1
    fi
  done
}

# cleanup_list -- the install-cleanup.txt to read: the root's, else the one
# shipped beside this script (bundle root or checkout root).
cleanup_list() {
  local c
  for c in "$ROOT_C/install-cleanup.txt" "$SELF_DIR/install-cleanup.txt" "$SELF_DIR/../install-cleanup.txt"; do
    if [ -f "$c" ] && [ ! -L "$c" ]; then printf '%s' "$c"; return 0; fi
  done
  return 1
}

# remove_retired -- the paths current Planar no longer ships (install-cleanup.txt),
# with install.sh's rules: plain relative paths only, never through a symlink,
# never a data path.
remove_retired() {
  local list raw line rel relpath rest p via target parent
  list="$(cleanup_list)" || return 0
  while IFS= read -r raw || [ -n "$raw" ]; do
    line="${raw%%#*}"
    read -r relpath rest <<EOF_LINE
$line
EOF_LINE
    [ -n "${relpath-}" ] || continue
    rel="${relpath%/}"
    case "/$rel/" in */../*|*/./*|//) continue ;; esac
    target="$ROOT_C/$rel"
    [ -e "$target" ] || [ -L "$target" ] || continue
    p="$ROOT_C"; via=0; rest="$rel"
    while :; do
      case "$rest" in */*) ;; *) break ;; esac
      p="$p/${rest%%/*}"; rest="${rest#*/}"
      if [ -L "$p" ]; then via=1; break; fi
    done
    [ "$via" -eq 0 ] || continue
    in_root_removable "$target" || continue
    remove_path "$target" "retired path" || continue
    parent="$(dirname "$target")"
    while [ "$parent" != "$ROOT_C" ]; do
      rmdir "$parent" 2>/dev/null || break
      parent="$(dirname "$parent")"
    done
  done < "$list"
}

remove_records() {
  local f
  for f in install-cleanup.txt release.json .planar-install install-manifest.json; do
    if [ -e "$ROOT_C/$f" ] || [ -L "$ROOT_C/$f" ]; then
      remove_path "$ROOT_C/$f" "install record" || true
    fi
  done
}

# ---------- data paths ----------

PLANAR_LEGACY_NAMES="agent.db agent.db-wal agent.db-shm"

# holds_relocated PATH -- PATH is, or holds, the relocated location of a data
# path; --purge keeps it.
holds_relocated() {
  local p name kind loc by lc
  p="$(planar_canonical_path "$1" 2>/dev/null || printf '%s' "$1")"
  while IFS='|' read -r name kind loc by; do
    [ -n "$by" ] || continue
    lc="$(planar_canonical_path "$loc" 2>/dev/null || printf '%s' "$loc")"
    case "$lc/" in "$p"/*) RELOC_HIT="$name ($by)"; return 0 ;; esac
  done <<EOF_DP
$(planar_data_paths_list "$ROOT_C")
EOF_DP
  return 1
}

# handle_data -- name every data path and the legacy agent.db; with --purge,
# remove the ones under the root that are not relocated.
handle_data() {
  local name kind loc by f
  RELOC_HIT=""
  while IFS='|' read -r name kind loc by; do
    [ -n "$name" ] || continue
    if [ -n "$by" ]; then
      case "$name" in planar.db-*) continue ;; esac
      log "kept relocated data path $name at $loc (relocated by $by): an uninstall never removes it, --purge included"
      if [ -e "$ROOT_C/$name" ] || [ -L "$ROOT_C/$name" ]; then
        log "kept $ROOT_C/$name: $by relocates $name, so the copy at the default location is left too"
      fi
      continue
    fi
    [ -e "$ROOT_C/$name" ] || [ -L "$ROOT_C/$name" ] || continue
    if [ "$PURGE" -eq 0 ]; then
      log "kept data path $ROOT_C/$name"
      continue
    fi
    if holds_relocated "$ROOT_C/$name"; then
      log "kept $ROOT_C/$name: it holds the relocated data path $RELOC_HIT"
      continue
    fi
    if [ "$name" = retired ] && [ -d "$ROOT_C/retired" ] && [ ! -L "$ROOT_C/retired" ]; then
      while IFS= read -r f; do
        [ -n "$f" ] && log "purging retired file $f"
      done <<EOF_RET
$(find "$ROOT_C/retired" \( -type f -o -type l \) 2>/dev/null | LC_ALL=C sort)
EOF_RET
    fi
    log "purging data path $ROOT_C/$name"
    remove_path "$ROOT_C/$name" "data path" || true
  done <<EOF_DP
$(planar_data_paths_list "$ROOT_C")
EOF_DP
  for name in $PLANAR_LEGACY_NAMES; do
    [ -e "$ROOT_C/$name" ] || [ -L "$ROOT_C/$name" ] || continue
    if [ "$PURGE" -eq 0 ]; then
      log "kept legacy queue database $ROOT_C/$name (the retired agent queue; --purge removes it)"
    else
      log "purging legacy queue database $ROOT_C/$name"
      remove_path "$ROOT_C/$name" "legacy queue database" || true
    fi
  done
}

# ---------- what is left ----------

# report_unknown -- every remaining top-level entry this uninstaller does not
# know is kept and reported.
report_unknown() {
  local e base
  for e in "$ROOT_C"/* "$ROOT_C"/.[!.]* "$ROOT_C"/..?*; do
    [ -e "$e" ] || [ -L "$e" ] || continue
    base="${e##*/}"
    case "$base" in
      .planar-journal|"$PLANAR_UNINSTALLED_NAME") continue ;;
    esac
    case " $PLANAR_LEGACY_NAMES " in *" $base "*) continue ;; esac
    if planar_removal_blocked "$ROOT_C" "$e" || holds_relocated "$e"; then continue; fi
    warn "kept $e: Planar does not know it, so the uninstall leaves it for you"
    KEPT_UNKNOWN=$((KEPT_UNKNOWN + 1))
  done
}

# root_has_entries -- the root holds something besides the journal and the marker.
root_has_entries() {
  local e base
  for e in "$ROOT_C"/* "$ROOT_C"/.[!.]* "$ROOT_C"/..?*; do
    [ -e "$e" ] || [ -L "$e" ] || continue
    base="${e##*/}"
    case "$base" in .planar-journal|"$PLANAR_UNINSTALLED_NAME") continue ;; esac
    return 0
  done
  return 1
}

# ---------- ~/.local/bin ----------

offer_local_bin() {
  local dir="$HOME/.local/bin" b found="" ans=""
  for b in $PLANAR_BINARY_NAMES; do
    if { [ -f "$dir/$b" ] || [ -L "$dir/$b" ]; } && [ ! -d "$dir/$b" ]; then found="$found $b"; fi
  done
  [ -n "$found" ] || return 0
  title "Planar binaries in $dir"
  for b in $found; do log "found $dir/$b (installed by the retired 'make install')"; done
  if [ "$YES" -eq 0 ]; then
    if [ -t 0 ]; then
      printf '  Remove them? [y/N] '
      IFS= read -r ans || ans=""
    else
      log "left them: stdin is not a terminal, so nothing was asked. Re-run with --yes to remove them."
      return 0
    fi
    case "$ans" in
      y|Y|yes|YES) ;;
      *) log "left them (not confirmed)"; return 0 ;;
    esac
  fi
  for b in $found; do remove_path "$dir/$b" "binary" || true; done
}

# ---------- the run ----------

title "Uninstalling Planar from $ROOT_C"
planar_data_paths_report "$ROOT_C" | while IFS= read -r _l; do log "$_l"; done

UNINSTALL_STARTED=0
RETRY=""
# shellcheck disable=SC2329  # the EXIT trap
_on_exit() {
  local rc=$?
  if [ "$rc" -ne 0 ] && [ "$UNINSTALL_STARTED" -eq 1 ] && [ -f "$ROOT_C/.planar-journal" ]; then
    printf '\nThe uninstall of %s stopped before it finished; the installation is partly removed. Finish it with:\n  %s\n' "$ROOT_C" "$RETRY" >&2
  fi
  planar_lock_release
  exit "$rc"
}

if [ -d "$ROOT_C" ]; then
  planar_lock_acquire "$ROOT_C" uninstall || die "$PLANAR_LOCK_ERROR"
  trap _on_exit EXIT
  if [ -n "$PLANAR_LOCK_RECLAIMED" ]; then log "reclaimed the mutation lock from an abandoned $PLANAR_LOCK_RECLAIMED"; fi
  planar_install_fault uninstall-after-lock || exit 1

  if [ -e "$ROOT_C/.planar-journal" ] || [ -L "$ROOT_C/.planar-journal" ]; then
    planar_journal_load "$ROOT_C" \
      || die "$ROOT_C/.planar-journal is not a valid recovery journal for $ROOT_C; nothing was removed and it is kept. Inspect it; remove it by hand only if no install or uninstall was interrupted, then re-run."
    if [ "$J_phase" != uninstalling ] && [ -n "${J_owner_pid-}" ] && [ "$J_owner_pid" != "$$" ] && [ -n "${J_owner_start-}" ] \
       && [ "$(planar_lock_start_token "$J_owner_pid" 2>/dev/null || true)" = "$J_owner_start" ]; then
      die "the interrupted install's owner (pid $J_owner_pid) is still running although it no longer holds the mutation lock; nothing was removed"
    fi
    case "$J_phase" in
      uninstalling) log "finishing an interrupted uninstall" ;;
      *) log "ending the pending install (journal phase $J_phase): it is cancelled, never resumed" ;;
    esac
  else
    planar_journal_clear
  fi

  RETRY="$(retry_command)"
  # Removals begin: record the cancellation, durably, before any of them. The
  # loaded staging and backup ownership is kept so a retry still knows them.
  J_phase=uninstalling
  # shellcheck disable=SC2034  # J_* are written to the journal by name
  J_operation=uninstall
  # shellcheck disable=SC2034  # J_* are written to the journal by name
  if [ -z "${J_operation_id-}" ]; then J_operation_id="$(_pl_nonce)" || J_operation_id=""; fi
  J_owner_pid="$$"
  J_owner_start="$(planar_lock_start_token "$$")" || J_owner_start=unknown
  # shellcheck disable=SC2034  # J_* are written to the journal by name
  J_owner_lock="$PLANAR_LOCK_GEN"
  # shellcheck disable=SC2034  # J_* are written to the journal by name
  J_retry="$RETRY"
  planar_journal_write "$ROOT_C" || die "cannot record the uninstall in $ROOT_C/.planar-journal; nothing was removed"
  sync || warn "sync failed; the uninstall record $ROOT_C/.planar-journal is written but may not be on disk yet"
  UNINSTALL_STARTED=1
  log "if this uninstall is interrupted, finish it with: $RETRY"
  planar_install_fault uninstall-recorded || exit 1

  title "Removing the vendor projections"
  remove_projections

  title "Removing the installation"
  remove_recovery
  remove_managed
  remove_retired
  planar_install_fault uninstall-before-records || exit 1
  remove_records

  if [ "$PURGE" -eq 1 ]; then title "Purging the data paths"; else title "Keeping the data paths"; fi
  handle_data
  report_unknown

  # The journal goes last. A root that keeps anything gets the marker first,
  # so the next install adopts it without --force.
  if root_has_entries; then
    planar_uninstalled_marker_write "$ROOT_C" \
      || warn "could not write $ROOT_C/$PLANAR_UNINSTALLED_NAME; a reinstall into $ROOT_C will need --force"
    rm -f "$ROOT_C/.planar-journal"
    log "kept $ROOT_C (it still holds the entries named above)"
  else
    rm -f "$ROOT_C/.planar-journal" "$ROOT_C/$PLANAR_UNINSTALLED_NAME"
    if rmdir "$ROOT_C" 2>/dev/null; then log "removed $ROOT_C (nothing was left in it)"; fi
  fi
  UNINSTALL_STARTED=0
  planar_lock_release
else
  log "nothing is installed at $ROOT_C"
fi

offer_local_bin

title "Uninstall complete."
[ "$PURGE" -eq 1 ] || log "the data paths named above were kept; planar-uninstall --purge removes them"
[ "$KEPT_UNKNOWN" -eq 0 ] || log "kept $KEPT_UNKNOWN entr$([ "$KEPT_UNKNOWN" -eq 1 ] && echo y || echo ies) Planar does not know (named above)"
[ "$WARN_COUNT" -eq 0 ] || log "$WARN_COUNT warning(s) above"
exit 0
