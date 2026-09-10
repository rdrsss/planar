#!/usr/bin/env python3
"""Task 6316: parser-refusal differential, CLI11 (C++) vs etcli-zig (oracle).

Measures — does not fix — how far CLI11's parser-refusal wording (decision
948) has drifted from the etcli-zig oracle it replaced, across every wired
leaf. See scripts/parity-data/6316-refusal-differential.jsonl for the
recorded output of the run this script produced, and task 6316 for the full
rationale (it exists to SIZE this, not to re-close it — task 6123 already
sanctioned re-baselining the wording).

For every wired leaf (a command with no subcommands) in each C++ binary's
`schema` JSON catalog, this drives both the C++ binary and its oracle
equivalent through the same set of parser-refusal arms, captures
stdout/stderr/exit separately, and classifies any divergence as:
  - wording  : same stream shape, same exit code, different text
  - stream   : same exit code, but stdout/stderr non-empty-ness differs
               (see task 6271 — parser refusals write the message to
               stdout and the tag to stderr; a handler refusal writes only
               to stderr; a leaf whose refusal path differs between the
               two binaries shows up here)
  - exit_code: both refuse (both nonzero) but at different exit codes
  - structural: one binary accepts what the other refuses (exit 0 vs
               nonzero) — the only category that is a real behavior
               difference rather than a cosmetic one

Refusal arms attempted, derived per-leaf from its own declared
flags/positionals (never assumed uniformly across leaves):
  - unknown_flag                 : always (append a flag no leaf declares)
  - missing_required_positional  : iff the leaf declares >=1 required
                                    positional
  - too_many_positionals         : iff the leaf declares ZERO positionals
                                    (so any positional token is unexpected)
  - bad_choice_value              : iff the leaf has a flag with a nonempty
                                    `choices` list
  - missing_flag_value           : iff the leaf has >=1 non-bool, non-list
                                    flag (a flag that needs a value)

Usage (from repo root, both trees already built):
    python3 scripts/parity-data/6316-differential-harness.py \
        build/debug/bin zig/zig-out/bin /tmp/6316-rerun.jsonl

Prerequisites: `cmake --build build/debug --target planar planar-agent
planar-watch planar-ext` and (in zig/) `zig build`.
"""
import json
import os
import subprocess
import sys
import tempfile
from collections import Counter

# (cpp_binary_name, oracle_binary_name) — planar-ext has no oracle binary of
# its own; its `ext`/`sync` leaves map onto the oracle's combined `planar`
# binary, which still hosts them pre-extraction (decisions 995-1001).
TARGETS = [
    ("planar", "planar"),
    ("planar-agent", "planar-agent"),
    ("planar-watch", "planar-watch"),
    ("planar-ext", "planar"),
]


def schema_json(bin_path, scratch):
    p = subprocess.run([bin_path, "schema"], capture_output=True, text=True, cwd=scratch, timeout=30)
    return json.loads(p.stdout)


def env_for(scratch, tag):
    d = dict(os.environ)
    d["PLANAR_DB"] = os.path.join(scratch, f"{tag}.db")
    d["PLANAR_CONFIG_PATH"] = os.path.join(scratch, f"{tag}.toml")
    return d


def run(bin_path, args, scratch, tag):
    try:
        p = subprocess.run(
            [bin_path] + args, capture_output=True, text=True, timeout=15, env=env_for(scratch, tag), cwd=scratch
        )
        return {"stdout": p.stdout, "stderr": p.stderr, "exit": p.returncode}
    except subprocess.TimeoutExpired:
        return {"stdout": "", "stderr": "TIMEOUT", "exit": -1}


def build_arms(leaf):
    """Return {arm_name: extra_args} for arms this leaf can exercise."""
    arms = {}
    flags = leaf.get("flags", [])
    positionals = leaf.get("positionals", [])

    arms["unknown_flag"] = ["--this-flag-does-not-exist-zzz9"]

    if any(p.get("required") for p in positionals):
        arms["missing_required_positional"] = []

    if not positionals:
        arms["too_many_positionals"] = ["unexpected-positional-arg"]

    choice_flags = [f for f in flags if f.get("choices")]
    if choice_flags:
        arms["bad_choice_value"] = [choice_flags[0]["long"], "definitely-not-a-valid-choice-zzz"]

    value_flags = [f for f in flags if f.get("kind") != "bool" and not f.get("list")]
    if value_flags:
        arms["missing_flag_value"] = [value_flags[0]["long"]]

    return arms


def base_args_for_required_positionals(leaf, skip):
    if skip:
        return []
    return ["placeholder-value-zzz" for p in leaf.get("positionals", []) if p.get("required")]


def classify(cpp_res, oracle_res):
    if cpp_res["exit"] != oracle_res["exit"]:
        if cpp_res["exit"] == 0 or oracle_res["exit"] == 0:
            return "structural"
        return "exit_code"
    shapes = tuple(bool(r[s].strip()) for r in (cpp_res, oracle_res) for s in ("stdout", "stderr"))
    if shapes[:2] != shapes[2:]:
        return "stream"
    if cpp_res["stdout"].strip() == oracle_res["stdout"].strip() and cpp_res["stderr"].strip() == oracle_res["stderr"].strip():
        return "match"
    return "wording"


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)
    cpp_bin_dir, oracle_bin_dir, out_path = (os.path.abspath(p) for p in sys.argv[1:4])

    results = []
    total_leaves = 0

    with tempfile.TemporaryDirectory(prefix="planar-6316-") as scratch:
        for cpp_name, oracle_name in TARGETS:
            cpp_bin = os.path.join(cpp_bin_dir, cpp_name)
            oracle_bin = os.path.join(oracle_bin_dir, oracle_name)

            cpp_schema = schema_json(cpp_bin, scratch)
            oracle_schema = schema_json(oracle_bin, scratch)
            oracle_by_path = {tuple(c["path"]): c for c in oracle_schema["commands"]}

            for c in cpp_schema["commands"]:
                if c["subcommands"]:
                    continue  # not a leaf
                path = tuple(c["path"])
                if path not in oracle_by_path:
                    results.append(
                        {
                            "cpp_binary": cpp_name,
                            "leaf": c["command"],
                            "arm": "N/A",
                            "classification": "no_oracle_equivalent",
                        }
                    )
                    continue

                total_leaves += 1
                arms = build_arms(c)
                tag_base = (cpp_name + "_" + "_".join(path))[:80]

                for arm_name, extra in arms.items():
                    skip_req_pos = arm_name == "missing_required_positional"
                    base = base_args_for_required_positionals(c, skip=skip_req_pos)
                    args = list(path) + base + extra

                    cpp_res = run(cpp_bin, args, scratch, tag_base + "_cpp")
                    oracle_res = run(oracle_bin, args, scratch, tag_base + "_oracle")

                    cls = classify(cpp_res, oracle_res)
                    if cls == "match":
                        continue
                    results.append(
                        {
                            "cpp_binary": cpp_name,
                            "leaf": c["command"],
                            "arm": arm_name,
                            "args": args,
                            "cpp_exit": cpp_res["exit"],
                            "oracle_exit": oracle_res["exit"],
                            "cpp_stdout": cpp_res["stdout"][:2000],
                            "cpp_stderr": cpp_res["stderr"][:2000],
                            "oracle_stdout": oracle_res["stdout"][:2000],
                            "oracle_stderr": oracle_res["stderr"][:2000],
                            "classification": cls,
                        }
                    )

    with open(out_path, "w") as f:
        for r in results:
            f.write(json.dumps(r) + "\n")

    print(f"total_leaves_measured={total_leaves}")
    for k, v in Counter(r["classification"] for r in results).most_common():
        print(f"{k}: {v}")
    print(f"written to {out_path}")


if __name__ == "__main__":
    main()
