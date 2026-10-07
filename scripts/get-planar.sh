#!/bin/sh
# get-planar.sh -- install a Planar release bundle with one command.
#
#   curl -fsSL https://github.com/rdrsss/planar/releases/latest/download/get-planar.sh | sh
#   curl -fsSL https://github.com/rdrsss/planar/releases/download/vX.Y.Z/get-planar.sh | PLANAR_VERSION=vX.Y.Z sh
#
# POSIX sh, no bashisms, no eval, no Python. It runs under a pipe, so there are
# no positional arguments: every input is an environment variable whose grammar
# is checked before it is used.
#
#   PLANAR_VERSION      a release tag, ^v[0-9]+\.[0-9]+\.[0-9]+$. Default: the
#                       content of <base>/latest/download/VERSION, which must
#                       match the same grammar.
#   PLANAR_RELEASE_URL  the release base. https://..., a local file:///...
#                       fixture, or http://127.0.0.1 / http://localhost with an
#                       optional numeric port. Default:
#                       https://github.com/rdrsss/planar/releases
#   PLANAR_HOME         the install root the installer uses. Default: ~/.planar.
#   TMPDIR              where the temporary directory goes. Default: /tmp.
#
# Contract (tech spec 677, "The bootstrap"): refuse any platform but macos-arm64
# (macOS 26 or later) and linux-x86_64; before choosing a release, let a pending
# interrupted install pin the release it was installing; fetch every asset from
# <base>/download/<tag>/ so a release published mid-run cannot mix two versions;
# verify SHA256SUMS before extracting; run install.sh --prebuilt in the
# foreground; remove the temporary directory only after the installer exits.
#
# Redirects are followed by this script, one hop at a time, never by the client:
# every hop's URL is checked against the same grammar as the base, an https hop
# may only lead to https, and an http fixture hop to http on the same loopback
# rules or to https. curl and wget run with redirect following off.
#
# Refusals go through die(), which names its subject, so each stays greppable.

LC_ALL=C
export LC_ALL

DEFAULT_RELEASE_BASE="https://github.com/rdrsss/planar/releases"
SOURCE_POINTER="build from source instead: https://github.com/rdrsss/planar (INSTALL.md, \"Build from source\")"
MAX_HOPS=10

TMP_DIR=""

# die MESSAGE... -- print the refusal and stop. The EXIT trap removes TMP_DIR.
die() {
  printf 'get-planar: %s\n' "$*" >&2
  exit 1
}

# shellcheck disable=SC2329  # runs from the EXIT trap
cleanup() {
  if [ -n "$TMP_DIR" ] && [ -d "$TMP_DIR" ]; then
    rm -rf "$TMP_DIR"
  fi
}

# printable TEXT -- TEXT cut to printable ASCII, for echoing untrusted data.
printable() {
  printf '%s' "$1" | tr -cd '[:print:]' | cut -c1-200
}

# shquote TEXT -- TEXT as one single-quoted shell word.
shquote() {
  printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

# ---------- grammars ----------

# version_valid TAG -- ^v[0-9]+\.[0-9]+\.[0-9]+$ (no newline can pass the filter).
version_valid() {
  case "$1" in
    ""|*[!0-9v.]*) return 1 ;;
  esac
  printf '%s\n' "$1" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+$'
}

# url_parse URL [QUERY_OK] -- split URL into U_SCHEME, U_AUTH and U_PATH, or
# fail. The scheme is https, file or http; http is accepted only for the exact
# hosts 127.0.0.1 and localhost; file takes no authority. Userinfo, whitespace,
# quotes, backslashes, fragments and malformed authorities fail. A query is
# kept only when QUERY_OK is yes (a redirect target may carry one; a base may
# not) and lands in U_QUERY.
url_parse() {
  U_SCHEME=""; U_AUTH=""; U_PATH=""; U_QUERY=""
  _up_url=$1
  case "$_up_url" in
    *\?*)
      [ "${2-}" = yes ] || return 1
      U_QUERY=${_up_url#*\?}
      _up_url=${_up_url%%\?*}
      case "$U_QUERY" in
        ""|*[!A-Za-z0-9._~:/%+\&=,\;-]*) return 1 ;;
      esac
      ;;
  esac
  case "$_up_url" in
    ""|*[!A-Za-z0-9._~:/%+-]*) return 1 ;;
  esac
  case "$_up_url" in
    https://*) U_SCHEME=https; _up_rest=${_up_url#https://} ;;
    http://*)  U_SCHEME=http;  _up_rest=${_up_url#http://} ;;
    file://*)  U_SCHEME="file";  _up_rest=${_up_url#file://} ;;
    *) return 1 ;;
  esac
  case "$_up_rest" in
    */*) U_AUTH=${_up_rest%%/*}; U_PATH=/${_up_rest#*/} ;;
    *)   U_AUTH=$_up_rest; U_PATH="" ;;
  esac
  if [ "$U_SCHEME" = file ]; then
    [ -z "$U_AUTH" ] && [ -n "$U_PATH" ] || return 1
    case "$U_PATH" in
      *%*|*/../*|*/..|*/./*|*/.) return 1 ;;
    esac
    return 0
  fi
  _up_host=$U_AUTH
  _up_port=""
  case "$U_AUTH" in
    *:*)
      _up_host=${U_AUTH%%:*}
      _up_port=${U_AUTH#*:}
      case "$_up_port" in
        ""|*[!0-9]*) return 1 ;;
      esac
      [ "${#_up_port}" -le 5 ] || return 1
      [ "$(printf '%s' "$_up_port" | sed 's/^0*//;s/^$/0/')" -le 65535 ] || return 1
      ;;
  esac
  case "$_up_host" in
    ""|*[!A-Za-z0-9.-]*|.*|*.|-*|*-|*..*) return 1 ;;
  esac
  if [ "$U_SCHEME" = http ]; then
    case "$_up_host" in
      127.0.0.1|localhost) ;;
      *) return 1 ;;
    esac
  fi
  return 0
}

# base_normalize URL -- URL without trailing slashes.
base_normalize() {
  _bn=$1
  while :; do
    case "$_bn" in
      */) _bn=${_bn%/} ;;
      *) break ;;
    esac
  done
  printf '%s' "$_bn"
}

# base_valid URL -- URL is a release base: url_parse without a query.
base_valid() {
  url_parse "$1" no
}

# hop_allowed FROM_SCHEME TO_SCHEME -- the redirect protocol rule: an https hop
# may only lead to https; an http fixture hop to http or https; file never
# redirects.
hop_allowed() {
  case "$1:$2" in
    https:https|http:http|http:https) return 0 ;;
    *) return 1 ;;
  esac
}

# ---------- platform ----------

# detect_platform -- PLATFORM is macos-arm64 or linux-x86_64, or die.
detect_platform() {
  _os=$(uname -s 2>/dev/null) || _os=""
  _arch=$(uname -m 2>/dev/null) || _arch=""
  case "$_os:$_arch" in
    Darwin:arm64)
      PLATFORM=macos-arm64
      _mac=$(sw_vers -productVersion 2>/dev/null) || _mac=""
      _major=${_mac%%.*}
      case "$_major" in
        ""|*[!0-9]*) die "cannot read the macOS version (sw_vers reported '$(printable "$_mac")'); Planar needs macOS 26.0 or later. $SOURCE_POINTER" ;;
      esac
      if [ "$_major" -lt 26 ]; then
        die "this is macOS $(printable "$_mac"); Planar release bundles need macOS 26.0 or later (supported: macos-arm64 on macOS 26.0+, linux-x86_64). $SOURCE_POINTER"
      fi
      ;;
    Linux:x86_64)
      PLATFORM=linux-x86_64
      ;;
    *)
      die "unsupported platform $(printable "$_os") $(printable "$_arch"); Planar release bundles exist for macos-arm64 (macOS 26.0 or later) and linux-x86_64 only. $SOURCE_POINTER"
      ;;
  esac
}

# ---------- inputs ----------

# read_inputs -- validate PLANAR_VERSION, PLANAR_RELEASE_URL and PLANAR_HOME.
# Sets WANT_VERSION (empty when unset), BASE, BASE_GIVEN (yes/no) and ROOT.
# Empty variables count as unset.
read_inputs() {
  WANT_VERSION=${PLANAR_VERSION-}
  if [ -n "$WANT_VERSION" ] && ! version_valid "$WANT_VERSION"; then
    die "PLANAR_VERSION='$(printable "$WANT_VERSION")' is not a release tag; it must match ^v[0-9]+\\.[0-9]+\\.[0-9]+\$ (for example v1.2.3)"
  fi
  BASE_GIVEN=no
  BASE=$DEFAULT_RELEASE_BASE
  if [ -n "${PLANAR_RELEASE_URL-}" ]; then
    BASE_GIVEN=yes
    BASE=$(base_normalize "$PLANAR_RELEASE_URL")
    base_valid "$BASE" \
      || die "PLANAR_RELEASE_URL='$(printable "$PLANAR_RELEASE_URL")' is not an accepted release base; it must be https://HOST[:PORT]/..., file:///PATH, or http://127.0.0.1[:PORT]/... or http://localhost[:PORT]/... with no userinfo"
  fi
  [ -n "${HOME-}" ] || die "HOME is not set"
  ROOT=${PLANAR_HOME-$HOME/.planar}
  case "$ROOT" in
    /*) ;;
    *) die "PLANAR_HOME='$(printable "$ROOT")' must be an absolute path" ;;
  esac
  case "$ROOT" in
    *[![:print:]]*) die "PLANAR_HOME holds a control character" ;;
  esac
}

# ---------- downloading ----------

# http_get URL DEST HEADERS ERRFILE -- one request with redirects off. Sets
# F_STATUS (the last status code, or empty) and F_LOC (a Location header, or
# empty). Status 0 when the client ran.
http_get() {
  _hg_scheme=${1%%:*}
  : > "$3"
  if [ "$CLIENT" = curl ]; then
    F_STATUS=$(curl -q -sS --connect-timeout 20 --max-time 600 --proto "=$_hg_scheme" \
      --max-redirs 0 -D "$3" -o "$2" -w '%{http_code}' "$1" 2>"$4") || { F_STATUS=""; return 1; }
    F_STATUS=$(printf '%s' "$F_STATUS" | tr -cd '0-9')
  else
    if [ "$_hg_scheme" = https ]; then _hg_only=--https-only; else _hg_only=""; fi
    # shellcheck disable=SC2086  # _hg_only is empty or one fixed option
    WGETRC="$TMP_DIR/wgetrc" wget -nv -S --max-redirect=0 --tries=1 --timeout=60 $WGET_NOHSTS $_hg_only \
      -O "$2" "$1" 2>"$4"
    # The headers go to stderr; the client's exit status is not the verdict.
    cp "$4" "$3" 2>/dev/null
    F_STATUS=$(sed -n 's/^[[:space:]]*HTTP\/[0-9.]*[[:space:]]\{1,\}\([0-9][0-9][0-9]\).*/\1/p' "$3" | tail -n 1)
  fi
  F_LOC=$(tr -d '\r' < "$3" | sed -n 's/^[[:space:]]*[Ll][Oo][Cc][Aa][Tt][Ii][Oo][Nn]:[[:space:]]*//p' | tail -n 1)
  return 0
}

# fetch URL DEST -- download URL to DEST, following redirects by hand under the
# redirect policy. Status 1 with F_REASON set on any failure; DEST is only
# trusted on status 0.
fetch() {
  _f_url=$1
  _f_dest=$2
  _f_hops=0
  _f_prev=""
  F_REASON=""
  rm -f "$_f_dest"
  while :; do
    if ! url_parse "$_f_url" yes; then
      F_REASON="a redirect led to an unacceptable URL ($(printable "$_f_url"))"
      return 1
    fi
    if [ -n "$_f_prev" ] && ! hop_allowed "$_f_prev" "$U_SCHEME"; then
      F_REASON="a redirect from $_f_prev to $U_SCHEME is refused ($(printable "$_f_url"))"
      return 1
    fi
    _f_prev=$U_SCHEME
    if [ "$U_SCHEME" = file ]; then
      if [ -f "$U_PATH" ] && cp "$U_PATH" "$_f_dest" 2>/dev/null; then
        return 0
      fi
      F_REASON="no such file $U_PATH"
      return 1
    fi
    _f_scheme=$U_SCHEME
    _f_auth=$U_AUTH
    http_get "$_f_url" "$_f_dest" "$TMP_DIR/headers" "$TMP_DIR/client-err" \
      || { F_REASON="the $CLIENT client failed: $(printable "$(cat "$TMP_DIR/client-err" 2>/dev/null)")"; rm -f "$_f_dest"; return 1; }
    case "$F_STATUS" in
      200)
        return 0
        ;;
      301|302|303|307|308)
        _f_hops=$((_f_hops + 1))
        if [ "$_f_hops" -gt "$MAX_HOPS" ]; then
          F_REASON="more than $MAX_HOPS redirects"
          rm -f "$_f_dest"
          return 1
        fi
        case "$F_LOC" in
          "") F_REASON="HTTP $F_STATUS without a Location header"; rm -f "$_f_dest"; return 1 ;;
          /[!/]*) _f_url="$_f_scheme://$_f_auth$F_LOC" ;;
          *) _f_url=$F_LOC ;;
        esac
        ;;
      *)
        if [ -n "$F_STATUS" ]; then
          F_REASON="HTTP $F_STATUS"
        else
          F_REASON="no HTTP response: $(printable "$(cat "$TMP_DIR/client-err" 2>/dev/null)")"
        fi
        rm -f "$_f_dest"
        return 1
        ;;
    esac
  done
}

# pick_client -- CLIENT is curl, else GNU wget, or die.
pick_client() {
  if command -v curl >/dev/null 2>&1; then
    CLIENT=curl
  elif command -v wget >/dev/null 2>&1; then
    # Only GNU wget can be told to leave redirects to this script.
    if wget --version 2>/dev/null | head -n 1 | grep -q 'GNU Wget'; then
      CLIENT=wget
      : > "$TMP_DIR/wgetrc"   # an empty config: ~/.wgetrc must not change redirect handling
      WGET_NOHSTS=""
      if wget --help 2>&1 | grep -q -- '--no-hsts'; then WGET_NOHSTS=--no-hsts; fi
    else
      die "found wget but it is not GNU Wget, which cannot be told not to follow redirects; install curl or GNU wget"
    fi
  else
    die "neither curl nor wget is installed; install one of them"
  fi
}

# ---------- recovery evidence ----------

# journal_scan -- read ROOT's recovery journal without trusting it. Sets
# JR_STATE: none (no journal), settled (prepared, aborted-before-mutation or
# complete: no pin), mutating, or uninstalling. For mutating it also sets
# JR_VERSION, JR_SHA and JR_BASE. A journal that is not a well-formed version-1
# journal for the canonical root dies: it is preserved, never interpreted.
journal_scan() {
  JR_STATE=none; JR_VERSION=""; JR_SHA=""; JR_BASE=""; JR_SOURCE=""
  _jf=$ROOT/.planar-journal
  if [ ! -e "$_jf" ] && [ ! -L "$_jf" ]; then
    return 0
  fi
  _croot=$(cd "$ROOT" 2>/dev/null && pwd -P) || die "cannot enter the install root $ROOT that holds a recovery journal"
  _bad="$_jf is not a valid recovery journal for $_croot; nothing was changed. Inspect it; remove it by hand only if no install was interrupted"
  [ ! -L "$_jf" ] && [ -f "$_jf" ] || die "$_bad"
  [ -n "$(find "$_jf" -type f -user "$(id -u)" -prune 2>/dev/null)" ] || die "$_bad"
  _size=$(wc -c < "$_jf" | tr -d ' ')
  [ -n "$_size" ] && [ "$_size" -le 65536 ] || die "$_bad"
  [ "$(tr -d '\011\012\040-\176\200-\377' < "$_jf" | wc -c | tr -d ' ')" = 0 ] || die "$_bad"
  _n=0; _seen=" "; _phase=""; _root=""
  _op=""
  while IFS= read -r _line || [ -n "$_line" ]; do
    _n=$((_n + 1))
    if [ "$_n" -eq 1 ]; then
      [ "$_line" = "planar-journal 1" ] || die "$_bad"
      continue
    fi
    case "$_line" in
      [a-z]*=*) ;;
      *) die "$_bad" ;;
    esac
    _key=${_line%%=*}
    _val=${_line#*=}
    case "$_key" in
      *[!a-z0-9_]*) die "$_bad" ;;
    esac
    case "$_seen" in
      *" $_key "*) die "$_bad" ;;
    esac
    _seen="$_seen$_key "
    case "$_key" in
      root) _root=$_val ;;
      phase) _phase=$_val ;;
      source) JR_SOURCE=$_val ;;
      operation) _op=$_val ;;
      target_version) JR_VERSION=$_val ;;
      target_sha) JR_SHA=$_val ;;
      release_base) JR_BASE=$_val ;;
    esac
  done < "$_jf"
  [ "$_root" = "$_croot" ] || die "$_bad"
  case "$_phase" in
    prepared|aborted-before-mutation|complete) JR_STATE=settled; return 0 ;;
    uninstalling) JR_STATE=uninstalling; return 0 ;;
    mutating) ;;
    *) die "$_bad" ;;
  esac
  JR_STATE=mutating
  if [ "$JR_SOURCE" != prebuilt ]; then
    die "an interrupted source install of $_croot is pending (journal $_jf); finish it by re-running install.sh from the checkout it was started in. This bootstrap installs releases only; nothing was changed"
  fi
  version_valid "$JR_VERSION" || die "$_bad (its target version is not a release tag)"
  printf '%s\n' "$JR_SHA" | grep -Eq '^[0-9a-f]{40}$' || die "$_bad (its target commit is not a full SHA)"
  JR_BASE=$(base_normalize "$JR_BASE")
  base_valid "$JR_BASE" || die "$_bad (its release base is not an accepted URL)"
  return 0
}

# recovery_command -- the one-line command that finishes the pending install.
recovery_command() {
  _rc_envs=" PLANAR_VERSION=$JR_VERSION"
  [ "$JR_BASE" = "$DEFAULT_RELEASE_BASE" ] || _rc_envs="$_rc_envs PLANAR_RELEASE_URL=$(shquote "$JR_BASE")"
  [ "$ROOT" = "$HOME/.planar" ] || _rc_envs="$_rc_envs PLANAR_HOME=$(shquote "$ROOT")"
  printf 'curl -fsSL %s/download/%s/get-planar.sh |%s sh' "$JR_BASE" "$JR_VERSION" "$_rc_envs"
}

# ---------- checksums ----------

# sha_check DIR RECORDFILE -- verify with shasum -a 256 -c or sha256sum -c.
sha_check() {
  if command -v sha256sum >/dev/null 2>&1; then
    (cd "$1" && sha256sum -c "$2" >/dev/null 2>&1)
  elif command -v shasum >/dev/null 2>&1; then
    (cd "$1" && shasum -a 256 -c "$2" >/dev/null 2>&1)
  else
    die "neither sha256sum nor shasum is installed; cannot verify $ASSET"
  fi
}

# select_record -- write the one checksum record for ASSET from SHA256SUMS to
# $TMP_DIR/asset.sha256, or die. Exactly one line may name the asset, with or
# without a path or binary marker; it must be exactly "<64 lowercase hex>  <asset>".
select_record() {
  _count=0
  _record=""
  while IFS= read -r _line || [ -n "$_line" ]; do
    _line=${_line%"$(printf '\r')"}
    case "$_line" in
      *"$ASSET")
        _count=$((_count + 1))
        _record=$_line
        ;;
    esac
  done < "$TMP_DIR/SHA256SUMS"
  [ "$_count" -ge 1 ] || die "SHA256SUMS has no checksum record for $ASSET; nothing was extracted or installed"
  [ "$_count" -eq 1 ] || die "SHA256SUMS has $_count checksum records for $ASSET; nothing was extracted or installed"
  _hash=${_record%%  *}
  [ "$_record" = "$_hash  $ASSET" ] && [ "${#_hash}" -eq 64 ] || die "the SHA256SUMS record for $ASSET is malformed or names a path; nothing was extracted or installed"
  case "$_hash" in
    *[!0-9a-f]*) die "the SHA256SUMS record for $ASSET holds a malformed hash; nothing was extracted or installed" ;;
  esac
  printf '%s\n' "$_record" > "$TMP_DIR/asset.sha256"
}

# release_field FILE KEY -- one value of the flat release.json.
release_field() {
  sed -n "s/^[[:space:]]*\"$2\":[[:space:]]*\"\{0,1\}\([^\",]*\)\"\{0,1\},\{0,1\}[[:space:]]*\$/\1/p" "$1" 2>/dev/null | head -n 1
}

# kept_note -- appended to a download failure of a recovery run.
kept_note() {
  if [ "$PINNED" = yes ]; then
    printf '; the interrupted install of %s is kept for another try' "$TAG"
  fi
}

# ---------- main ----------

main() {
  [ "$#" -eq 0 ] || die "takes no arguments; set PLANAR_VERSION or PLANAR_RELEASE_URL in the environment (see the header of this script)"
  read_inputs
  detect_platform
  ASSET="planar-$PLATFORM.tar.gz"

  # Evidence first, from a journal this script validates itself.
  journal_scan
  PINNED=no
  EXPECT_SHA=""
  case "$JR_STATE" in
    uninstalling)
      die "an uninstall of $ROOT was interrupted; an install never resumes a cancelled installation. Finish the uninstall first: run $ROOT/bin/planar-uninstall (or uninstall.sh from a release bundle). Nothing was changed"
      ;;
    mutating)
      if [ -n "$WANT_VERSION" ] && [ "$WANT_VERSION" != "$JR_VERSION" ]; then
        die "PLANAR_VERSION=$WANT_VERSION conflicts with the interrupted install of $JR_VERSION in $ROOT, which must finish first. Run: $(recovery_command)"
      fi
      if [ "$BASE_GIVEN" = yes ] && [ "$BASE" != "$JR_BASE" ]; then
        die "PLANAR_RELEASE_URL=$BASE conflicts with the release base $JR_BASE recorded by the interrupted install of $JR_VERSION in $ROOT, which must finish first. Run: $(recovery_command)"
      fi
      PINNED=yes
      TAG=$JR_VERSION
      BASE=$JR_BASE
      EXPECT_SHA=$JR_SHA
      ;;
  esac

  TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/planar-get.XXXXXX") || die "cannot create a temporary directory under ${TMPDIR:-/tmp}"
  trap cleanup EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  pick_client

  if [ "$PINNED" = yes ]; then
    printf 'get-planar: finishing the interrupted install of %s (commit %s) from %s\n' "$TAG" "$EXPECT_SHA" "$BASE"
  elif [ -n "$WANT_VERSION" ]; then
    TAG=$WANT_VERSION
  else
    fetch "$BASE/latest/download/VERSION" "$TMP_DIR/VERSION" \
      || die "cannot read the latest release from $BASE/latest/download/VERSION: $F_REASON"
    [ "$(wc -c < "$TMP_DIR/VERSION" | tr -d ' ')" -le 64 ] || die "$BASE/latest/download/VERSION is too large to be a release tag"
    TAG=$(head -n 1 "$TMP_DIR/VERSION" | tr -d '\r')
    version_valid "$TAG" || die "$BASE/latest/download/VERSION holds '$(printable "$TAG")', which is not a release tag (^v[0-9]+\\.[0-9]+\\.[0-9]+\$)"
  fi

  ASSETS="$BASE/download/$TAG"
  printf 'get-planar: installing Planar %s for %s\n' "$TAG" "$PLATFORM"
  fetch "$ASSETS/SHA256SUMS" "$TMP_DIR/SHA256SUMS" \
    || die "cannot download SHA256SUMS for $TAG from $ASSETS/SHA256SUMS: $F_REASON$(kept_note)"
  fetch "$ASSETS/$ASSET" "$TMP_DIR/$ASSET" \
    || die "cannot download $ASSET for $TAG from $ASSETS/$ASSET: $F_REASON$(kept_note)"

  select_record
  sha_check "$TMP_DIR" asset.sha256 || die "checksum mismatch for $ASSET: the download does not match SHA256SUMS; nothing was extracted or installed"

  BUNDLE_NAME="planar-$PLATFORM"
  tar -tzf "$TMP_DIR/$ASSET" > "$TMP_DIR/entries" 2>/dev/null || die "$ASSET is not a readable tar archive; nothing was installed"
  if grep -Ev "^$BUNDLE_NAME(/|\$)" "$TMP_DIR/entries" | grep -q . || grep -Eq '(^|/)\.\.(/|$)' "$TMP_DIR/entries"; then
    die "$ASSET holds entries outside $BUNDLE_NAME/; nothing was extracted or installed"
  fi
  mkdir "$TMP_DIR/x" || die "cannot create $TMP_DIR/x"
  tar -xzf "$TMP_DIR/$ASSET" -C "$TMP_DIR/x" || die "cannot extract $ASSET; nothing was installed"
  BUNDLE="$TMP_DIR/x/$BUNDLE_NAME"
  [ -f "$BUNDLE/install.sh" ] && [ -f "$BUNDLE/release.json" ] || die "$ASSET is not a release bundle (no install.sh or release.json); nothing was installed"
  _bv=$(release_field "$BUNDLE/release.json" version)
  [ "$_bv" = "$TAG" ] || die "$ASSET holds Planar '$(printable "$_bv")' but was fetched as $TAG; nothing was installed"
  if [ "$PINNED" = yes ]; then
    _bs=$(release_field "$BUNDLE/release.json" sha)
    [ "$_bs" = "$EXPECT_SHA" ] || die "$ASSET for $TAG holds commit '$(printable "$_bs")' but the interrupted install recorded $EXPECT_SHA; nothing was changed"
    # Revalidate just before replay. The authoritative check is the installer's,
    # under the mutation lock; this narrows the window and refuses early.
    _was=$JR_VERSION:$JR_SHA:$JR_BASE
    journal_scan
    [ "$JR_STATE" = mutating ] && [ "$_was" = "$JR_VERSION:$JR_SHA:$JR_BASE" ] \
      || die "the recovery state of $ROOT changed while $ASSET was downloading; refusing to replay stale evidence. Run the command again"
  fi

  command -v bash >/dev/null 2>&1 || die "bash is required to run the installer"
  # The installer reads its input from the environment it is given, not from ours.
  unset PLANAR_MUTATION_HANDOFF
  PLANAR_RELEASE_URL=$BASE
  export PLANAR_RELEASE_URL
  set -- --prebuilt "$BUNDLE"
  bash "$BUNDLE/install.sh" "$@" < /dev/null
  _irc=$?
  if [ "$_irc" -ne 0 ]; then
    printf 'get-planar: the installer exited with status %s\n' "$_irc" >&2
    exit "$_irc"
  fi
  if [ "$PINNED" = yes ]; then
    printf 'get-planar: finished the interrupted install of %s. Run the command again to check for a newer release.\n' "$TAG"
  fi

  _found=$(command -v planar 2>/dev/null) || _found=""
  if [ "$_found" != "$ROOT/bin/planar" ]; then
    if [ -n "$_found" ]; then
      printf 'get-planar: note: "planar" on your PATH is %s, but this install is %s/bin/planar. Put %s/bin first on PATH to use it.\n' "$_found" "$ROOT" "$ROOT"
    else
      printf 'get-planar: note: "planar" is not on your PATH; this install is %s/bin/planar. Add %s/bin to PATH.\n' "$ROOT" "$ROOT"
    fi
  fi
  exit 0
}

main "$@"
