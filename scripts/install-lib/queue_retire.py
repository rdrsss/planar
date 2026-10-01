#!/usr/bin/env python3
"""queue_retire.py -- install.sh's reader for the queue upgrade (plan 1089).

install.sh sources ``scripts/install-lib/queue-retire.sh`` and runs this file
as ``python3 queue_retire.py <command> ...`` (tech spec 656, "Install and
upgrade"). Every path arrives through ``sys.argv``; none is ever interpolated
into source.

Commands:

``probe-verdict <exit-code>``
    Classifies one ``planar-agent queue status 1 --json`` probe of the
    prefix ``planar.db``. The probe's stdout arrives on stdin and its exit
    code as the argument. Prints one line, ``<verdict>\\t<detail>``, where
    the verdict is ``usable``, ``behind``, ``incompatible``, ``foreign`` or
    ``failed``. Anything the probe table does not name, including output
    that is not JSON, is ``failed``: the caller aborts before retiring
    anything.

The module uses only the Python standard library. It shells out to no
program.
"""

import json
import sys

# Exit codes of this script. A verdict is printed with EXIT_OK; a misuse of
# the script itself (a bad argument) exits EXIT_FAILED, which the shell treats
# like a failed probe.
EXIT_OK = 0
EXIT_FAILED = 2

# `planar-agent` exit codes the probe table reads (tech spec 656, "Store and
# open path"): 125 for every queue refusal, 1 for `not_found` and for a parse
# failure, 2 for `invalid_input`.
_EXIT_QUEUE_REFUSED = 125
_EXIT_NOT_FOUND = 1

# The 125 tags that do not abort the install, and the verdict each maps to.
_REFUSAL_VERDICTS = {
    "schema_version_behind": "behind",
    "queue_schema_incompatible": "incompatible",
    "queue_schema_foreign": "foreign",
}


def _one_line(text):
    """Collapse ``text`` onto one line so a verdict stays one line."""
    return " ".join(str(text).split())


def classify_probe(exit_code, stdout_text):
    """Classify one probe from its exit code and its stdout.

    Returns ``(verdict, detail)``. The mapping is the probe table of tech
    spec 656, "Steps, in order", step 3: exit 0 with a status object, or exit
    1 with tag ``not_found``, is usable; exit 125 with one of the three tags
    in ``_REFUSAL_VERDICTS`` is that verdict; everything else is ``failed``.
    """
    try:
        document = json.loads(stdout_text)
    except ValueError:
        document = None
    tag = None
    if isinstance(document, dict):
        error = document.get("error")
        if isinstance(error, dict) and isinstance(error.get("tag"), str):
            tag = error["tag"]

    if exit_code == 0 and isinstance(document, dict) and "error" not in document:
        return "usable", "exit 0 with a status object"
    if exit_code == _EXIT_NOT_FOUND and tag == "not_found":
        return "usable", "exit 1, tag not_found"
    if exit_code == _EXIT_QUEUE_REFUSED and tag in _REFUSAL_VERDICTS:
        return _REFUSAL_VERDICTS[tag], "exit 125, tag " + tag

    if document is None:
        shape = "output that is not JSON"
    elif tag is None:
        shape = "JSON with no error tag"
    else:
        shape = "tag " + tag
    return "failed", "exit {} with {}".format(exit_code, shape)


def _cmd_probe_verdict(args):
    if len(args) != 1:
        print("usage: queue_retire.py probe-verdict <exit-code> < probe-stdout", file=sys.stderr)
        return EXIT_FAILED
    try:
        exit_code = int(args[0])
    except ValueError:
        print("queue_retire.py: probe-verdict: exit code {!r} is not an integer".format(args[0]), file=sys.stderr)
        return EXIT_FAILED
    raw = sys.stdin.buffer.read()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        text = ""
    verdict, detail = classify_probe(exit_code, text)
    print("{}\t{}".format(verdict, _one_line(detail)))
    return EXIT_OK


_COMMANDS = {
    "probe-verdict": _cmd_probe_verdict,
}


def main(argv):
    if len(argv) < 2 or argv[1] not in _COMMANDS:
        print("usage: queue_retire.py {} ...".format("|".join(sorted(_COMMANDS))), file=sys.stderr)
        return EXIT_FAILED
    return _COMMANDS[argv[1]](argv[2:])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
