#!/usr/bin/env python3
"""Generate src/cmd/<bin>/surface.cpp from the Zig oracle's `<bin> schema`.

Run from the repo root AFTER dumping the oracle catalogs.  The output is
checked in; regenerate by re-running this script.
"""
import json, sys, os, re

SCRATCH = sys.argv[1]
ROOT = os.getcwd()

BINS = {
    "planar":       ("planar.cmd.planar.surface",       "planar::cmd",        "src/cmd/planar/surface.cpp"),
    "planar-agent": ("planar.cmd.planar_agent.surface", "planar::cmd::agent", "src/cmd/planar-agent/surface.cpp"),
    "planar-watch": ("planar.cmd.planar_watch.surface", "planar::cmd::watch", "src/cmd/planar-watch/surface.cpp"),
}

DISPATCH = {
    "planar":       "src/cmd/planar/dispatch.cpp",
    "planar-agent": "src/cmd/planar-agent/dispatch.cpp",
    "planar-watch": "src/cmd/planar-watch/dispatch.cpp",
}

# Nodes that carry subcommands AND their own handler in the oracle, measured
# by invoking each group node against a scratch arena (see the task report).
DUAL = {"planar": ["resume", "handoff", "health"], "planar-agent": [], "planar-watch": []}


def esc(s: str) -> str:
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ord(ch) < 0x20:
            out.append("\\%03o" % ord(ch))
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def implemented_keys(path):
    text = open(os.path.join(ROOT, path)).read()
    return set(re.findall(r'table\.emplace\("([^"]*)"', text))


def gen(binname):
    module, ns, outpath = BINS[binname]
    cmds = json.load(open(os.path.join(SCRATCH, "oracle-%s.json" % binname)))["commands"]
    by_path = {c["command"]: c for c in cmds}
    # STABLE sort by depth only.  The catalog is a pre-order walk, so its
    # natural order already lists every parent before its children AND every
    # sibling in declaration order; a depth-stable sort keeps the sibling
    # order (which is what the oracle's help pages and `subcommands` arrays
    # show) while guaranteeing the parent-before-child invariant
    # `apply_surface` needs.  Sorting by path text instead would silently
    # alphabetise every group's children.
    nodes = [c for c in cmds if c["path"]]
    nodes.sort(key=lambda c: len(c["path"]))

    impl = implemented_keys(DISPATCH[binname])

    flag_arrays, pos_arrays, path_arrays = [], [], []
    entries = []
    for i, c in enumerate(nodes):
        key = " ".join(c["path"])
        # Path literal.
        path_arrays.append("constexpr std::string_view k_path_%d[] = {%s};" % (i, ", ".join(esc(p) for p in c["path"])))
        local = [f for f in c["flags"] if f["source"] == "local"]
        if local:
            rows = []
            for f in local:
                # The oracle reports `"default": false` for every bool flag;
                # CLI11's `add_flag` has no default string and the existing
                # hand-written declarations emit `null` there. Only value
                # flags carry a default through.
                dflt = f["default"]
                if isinstance(dflt, bool):
                    dflt = esc("true") if dflt else '{}'
                elif dflt is None or dflt == "":
                    dflt = '{}'
                else:
                    dflt = esc(str(dflt))
                rows.append("    {.name = %s, .kind = %s, .required = %s, .list = %s, .default_value = %s, .description = %s}," % (
                    esc(f["long"]), esc(f["kind"]), "true" if f["required"] else "false",
                    "true" if f["list"] else "false", dflt, esc(f["description"])))
            flag_arrays.append("constexpr flag_spec k_flags_%d[] = {\n%s\n};" % (i, "\n".join(rows)))
            fref = "k_flags_%d" % i
        else:
            fref = "{}"
        if c["positionals"]:
            rows = []
            for p in c["positionals"]:
                rows.append("    {.name = %s, .required = %s, .description = %s}," % (
                    esc(p["name"]), "true" if p["required"] else "false", esc(p["description"])))
            pos_arrays.append("constexpr positional_spec k_pos_%d[] = {\n%s\n};" % (i, "\n".join(rows)))
            pref = "k_pos_%d" % i
        else:
            pref = "{}"
        entries.append("      {.path = k_path_%d,\n       .description = %s,\n       .flags = %s,\n       .positionals = %s,\n       .group = %s}," % (
            i, esc(c["description"]), fref, pref, "true" if c["subcommands"] else "false"))

    # summaries
    sums = []
    for c in cmds:
        sums.append("    {%s, %s}," % (esc(c["command"]), esc(c["summary"])))

    # not-implemented inventory: leaves + duals, minus what dispatch registers.
    leaves = [" ".join(c["path"]) for c in nodes if not c["subcommands"]]
    unported = sorted(set(leaves + DUAL[binname]) - impl)
    ni = "\n".join("    %s," % esc(k) for k in unported)

    if unported:
        unported_body = f'''  static constexpr std::string_view k_unported[] = {{
{ni}
  }};
  return k_unported;'''
    else:
        # A zero-bound array cannot form a span with libc++.  Keep the
        # named initializer (the retirement evidence scanner reads it) but
        # return a zero-length view over a non-string sentinel.
        unported_body = '''  static constexpr std::string_view k_unported[] = {std::string_view{}};
  return std::span<std::string_view const>{k_unported}.first(0);'''

    body = f'''/// @file surface.cpp
/// @brief Implementation of `{module}` — GENERATED, do not hand-edit.
///
/// Regenerate with `scripts/gen-cli-surface.py` (see that script and this
/// module's interface header for the provenance argument).

module {module};

import std;
import planar.cliapp.surface;

namespace {ns} {{

using cliapp::flag_spec;
using cliapp::node_spec;
using cliapp::positional_spec;

namespace {{

{chr(10).join(path_arrays)}

{chr(10).join(flag_arrays)}

{chr(10).join(pos_arrays)}

}} // namespace

auto surface_nodes() -> std::vector<node_spec> {{
  return {{
{chr(10).join(entries)}
  }};
}}

auto surface_summaries() -> std::span<std::pair<std::string_view, std::string_view> const> {{
  static constexpr std::pair<std::string_view, std::string_view> k_summaries[] = {{
{chr(10).join(sums)}
  }};
  return k_summaries;
}}

auto unported_paths() -> std::span<std::string_view const> {{
{unported_body}
}}

}} // namespace {ns}
'''
    open(os.path.join(ROOT, outpath), "w").write(body)
    print(binname, "nodes", len(nodes), "leaves", len(leaves), "unported", len(unported), "->", outpath)


for b in BINS:
    gen(b)
