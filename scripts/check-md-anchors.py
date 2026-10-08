#!/usr/bin/env python3
"""check-md-anchors.py -- fail on a broken in-page anchor in a Markdown file.

Usage: check-md-anchors.py FILE...      check each file's `](#anchor)` links
       check-md-anchors.py --self-test  check the slug rules and the detector

A link `[text](#frag)` is broken when no heading of the same file has the
GitHub slug `frag` (and no `<a id|name="frag">` exists). Slugs follow GitHub:
lowercase, inline markup and punctuation removed (letters, digits, `_`, `-`
and spaces kept), each space becomes `-`, and a repeated heading gets `-1`,
`-2`, ... Fenced code blocks are skipped for headings and links alike.
Exit 0 clean, 1 on a broken anchor (each reported as FILE:LINE), 2 on usage.
"""
import re
import sys


def slug(text):
    parts = text.split("`")                                  # odd parts are code spans: kept literally
    for i in range(0, len(parts), 2):
        parts[i] = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", parts[i])  # links keep their text
        parts[i] = re.sub(r"<[^>]+>", "", parts[i])          # inline html outside code
    text = "".join(parts).strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)                     # \w is unicode-aware
    return text.replace(" ", "-")


def scan(lines):
    """Return (anchors, links) where links is a list of (lineno, fragment)."""
    anchors, seen, links = set(), {}, []
    fence = None
    for no, line in enumerate(lines, 1):
        m = re.match(r"^\s*(`{3,}|~{3,})", line)
        if m:
            if fence is None:
                fence = m.group(1)[0]
            elif m.group(1)[0] == fence:
                fence = None
            continue
        if fence:
            continue
        h = re.match(r"^ {0,3}#{1,6}[ \t]+(.*?)(?:[ \t]+#+)?[ \t]*$", line)
        if h:
            base = slug(h.group(1))
            n = seen.get(base, 0)
            seen[base] = n + 1
            anchors.add(base if n == 0 else "%s-%d" % (base, n))
        for a in re.finditer(r"<a\s[^>]*?(?:id|name)=[\"']([^\"']+)[\"']", line):
            anchors.add(a.group(1))
        stripped = re.sub(r"`[^`]*`", "", line)              # not links inside code spans
        for lk in re.finditer(r"\]\(#([^)\s]*)\)", stripped):
            links.append((no, lk.group(1)))
    return anchors, links


def broken(lines):
    anchors, links = scan(lines)
    return [(no, frag) for no, frag in links if frag not in anchors]


def self_test():
    doc = [
        "# Top", "## `planar task touches add <task-id> <repo-slug> [--path <p>]`", "## Dup", "## Dup",
        "### `tree(1)` flags with no Planar analog", "```", "# not a heading", "[x](#nope)", "```",
        "ok [a](#top) [b](#planar-task-touches-add-task-id-repo-slug---path-p) [c](#dup-1) [d](#tree1-flags-with-no-planar-analog)",
    ]
    assert broken(doc) == [], broken(doc)
    bad = doc + ["see [e](#deliberately-omitted-flags) and [f](#dup-2) and [g](#not-a-heading)"]
    assert [f for _, f in broken(bad)] == ["deliberately-omitted-flags", "dup-2", "not-a-heading"], broken(bad)
    print("check-md-anchors: self-test passed")


def main(argv):
    if argv == ["--self-test"]:
        self_test()
        return 0
    if not argv:
        print(__doc__.split("\n\n")[0], file=sys.stderr)
        return 2
    bad = 0
    for path in argv:
        with open(path, encoding="utf-8") as fh:
            lines = fh.read().split("\n")
        for no, frag in broken(lines):
            print("%s:%d: broken in-page anchor #%s" % (path, no, frag), file=sys.stderr)
            bad += 1
    if bad == 0:
        print("check-md-anchors: no broken in-page anchors in %s" % ", ".join(argv))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
