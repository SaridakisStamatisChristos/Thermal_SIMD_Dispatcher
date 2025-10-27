#!/usr/bin/env python3
"""Simple performance gate for the perf_smoke benchmark."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
from typing import List


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--baseline",
        required=True,
        type=pathlib.Path,
        help="Path to the baseline file containing the expected microseconds per evaluation.",
    )
    parser.add_argument(
        "--threshold",
        type=float,
        default=0.20,
        help="Maximum allowed relative regression (e.g. 0.15 for 15%%).",
    )
    parser.add_argument(
        "command",
        nargs=argparse.REMAINDER,
        help="Command to run (prefix with -- to terminate the argument parser).",
    )
    args = parser.parse_args(argv)
    if not args.command:
        parser.error("a benchmark command must be provided after --")
    if args.command[0] == "--":
        args.command = args.command[1:]
    if not args.command:
        parser.error("a benchmark command must be provided after --")
    return args


def read_baseline(path: pathlib.Path) -> float:
    try:
        contents = path.read_text(encoding="utf-8").strip()
    except OSError as exc:
        raise SystemExit(f"failed to read baseline '{path}': {exc}") from exc
    try:
        return float(contents)
    except ValueError as exc:
        raise SystemExit(f"baseline '{path}' does not contain a valid float") from exc


def extract_metric(stdout: str) -> float:
    for line in stdout.splitlines():
        if line.startswith("PERF_SMOKE_PER_EVAL_US="):
            try:
                return float(line.split("=", 1)[1])
            except ValueError as exc:
                raise SystemExit("failed to parse benchmark output") from exc
    raise SystemExit("benchmark output did not contain PERF_SMOKE_PER_EVAL_US")


def main(argv: List[str]) -> int:
    args = parse_args(argv)
    baseline = read_baseline(args.baseline)

    result = subprocess.run(args.command, capture_output=True, text=True)
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode != 0:
        return result.returncode

    measured = extract_metric(result.stdout)
    if baseline <= 0:
        raise SystemExit("baseline value must be positive")

    regression = (measured - baseline) / baseline
    print(f"Recorded perf_smoke: {measured:.3f} us (baseline {baseline:.3f} us)")
    print(f"Relative change: {regression * 100.0:.2f}% (threshold {args.threshold * 100.0:.2f}%)")

    if regression > args.threshold:
        print(
            f"Regression of {regression * 100.0:.2f}% exceeds allowed threshold",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
