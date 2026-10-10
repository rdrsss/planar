# shellcheck shell=bash
# shellcheck disable=SC2034  # both lists are read by the scripts that source this file
#
# managed-lists.sh -- the two name lists the installer, the uninstaller and the
# ownership rules must agree on (plan 1133, task rel-m5-installer-dedup). They
# were copied into install.sh, scripts/uninstall.sh and ownership.sh; this file
# is the one copy each of them sources.
#
#   PLANAR_JOURNAL_SUBTREES  the managed subtrees of the install root, in the
#                            order the installer stages, swaps and journals them.
#                            templates/ is a data path and is not listed.
#   PLANAR_VENDOR_NAMES      the vendors Planar places surfaces into, in the order
#                            they are reported.
#
# Both are space-separated words (bash 3.2: no associative arrays). Sourced,
# never executed.

PLANAR_JOURNAL_SUBTREES="bin skills agents codex-agents workflows scripts migrations"
PLANAR_VENDOR_NAMES="claude codex copilot gemini antigravity opencode"
