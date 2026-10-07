# shellcheck shell=bash
#
# release.sh -- readers of a release record shared by install.sh and the
# uninstaller (plan 1122; tech spec 677, "The bundle" and "The uninstaller").
#
# Sourced, never executed. Bash 3.2 and base utilities only; no python3.
#
#   release_field FILE KEY
#       Print one value of a release.json written one `"key": value` pair per
#       line (scripts/dist.sh), or nothing when the key is absent.
#   release_base_valid URL
#       Status 0 when URL is in the bootstrap's release-base grammar: https, a
#       local file:// fixture, or http on the exact loopback hosts, with no
#       userinfo, quote or blank.

DEFAULT_RELEASE_BASE="https://github.com/rdrsss/planar/releases"

release_field() {
  sed -n "s/^[[:space:]]*\"$2\":[[:space:]]*\"\{0,1\}\([^\",]*\)\"\{0,1\},\{0,1\}[[:space:]]*\$/\1/p" "$1" 2>/dev/null | head -n 1
}

release_base_valid() {
  [[ "$1" =~ ^(https://[A-Za-z0-9.-]+(:[0-9]+)?(/[^[:space:]\"\'@]*)?|file:///[^[:space:]\"\'@]*|http://(127\.0\.0\.1|localhost)(:[0-9]+)?(/[^[:space:]\"\'@]*)?)$ ]]
}
