#!/usr/bin/env python3
"""Planar's stable CLI adapter for the official Mt-KaHyPar Python wheel."""

from __future__ import annotations

import argparse
import multiprocessing
import os
from pathlib import Path
import sys

try:
    import mtkahypar
except Exception as exc:  # pragma: no cover - exercised by installer smoke test
    print(f"mtkahypar: native module unavailable: {exc}", file=sys.stderr)
    raise SystemExit(1)


VERSION = "1.6.1"


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(add_help=False)
    result.add_argument("--help", action="store_true")
    result.add_argument("--probe", action="store_true")
    result.add_argument("--version", action="store_true")
    result.add_argument("-h", dest="hypergraph")
    result.add_argument("-k", dest="blocks", type=int)
    result.add_argument("-e", dest="epsilon", type=float, default=0.03)
    result.add_argument("-o", dest="objective", default="km1")
    # Kept for compatibility with Planar's original subprocess contract.
    result.add_argument("-m", dest="mode", default="direct")
    result.add_argument("--write-partition-file", nargs="?", const="true", default="true")
    result.add_argument("--partition-output-folder", dest="output_folder")
    return result


def print_help() -> None:
    print(
        "mtkahypar (Planar wheel adapter)\n"
        "usage: mtkahypar -h GRAPH -k BLOCKS [-e EPSILON] [-o km1] "
        "[--write-partition-file=true] --partition-output-folder DIR"
    )


def thread_count() -> int:
    detected = multiprocessing.cpu_count()
    configured = os.environ.get("PLANAR_MTKAHYPAR_THREADS")
    if configured is None:
        return max(1, min(detected, 8))
    try:
        return max(1, min(detected, int(configured)))
    except ValueError:
        print("mtkahypar: PLANAR_MTKAHYPAR_THREADS must be an integer", file=sys.stderr)
        raise SystemExit(2)


def partition(args: argparse.Namespace) -> int:
    if args.hypergraph is None or args.blocks is None or args.output_folder is None:
        print_help()
        return 2
    if args.blocks < 1:
        print("mtkahypar: -k must be at least 1", file=sys.stderr)
        return 2
    objectives = {
        "cut": mtkahypar.Objective.CUT,
        "km1": mtkahypar.Objective.KM1,
        "soed": mtkahypar.Objective.SOED,
    }
    objective = objectives.get(args.objective.lower())
    if objective is None:
        print("mtkahypar: -o must be cut, km1, or soed", file=sys.stderr)
        return 2

    initializer = mtkahypar.initialize(thread_count())
    context = initializer.context_from_preset(mtkahypar.PresetType.DEFAULT)
    context.set_partitioning_parameters(args.blocks, args.epsilon, objective)
    mtkahypar.set_seed(0)

    hypergraph_path = Path(args.hypergraph)
    output_folder = Path(args.output_folder)
    output_folder.mkdir(parents=True, exist_ok=True)
    hypergraph = initializer.hypergraph_from_file(
        str(hypergraph_path), context, mtkahypar.FileFormat.HMETIS
    )
    result = hypergraph.partition(context)
    output = output_folder / (
        f"{hypergraph_path.name}.part{args.blocks}.epsilon{args.epsilon}.seed0.KaHyPar"
    )
    result.write_partition_to_file(str(output))
    return 0


def main() -> int:
    args, unknown = parser().parse_known_args()
    if unknown:
        print(f"mtkahypar: unsupported arguments: {' '.join(unknown)}", file=sys.stderr)
        return 2
    if args.version:
        print(f"mtkahypar {VERSION} (Planar wheel adapter)")
        return 0
    if args.probe:
        # Importing the native extension above is the meaningful availability
        # check; a stale or ABI-incompatible environment fails before this.
        return 0
    if args.help:
        print_help()
        return 0
    try:
        return partition(args)
    except Exception as exc:
        print(f"mtkahypar: partition failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
