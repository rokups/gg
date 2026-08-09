#!/usr/bin/env python3
# Copyright (c) 2026-2026 the gg project.
# This work is licensed under the terms of the GNU General Public License version 2.
# For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

"""Run gg's tests and require 100% reachable line and branch coverage."""

from __future__ import annotations

import argparse
import gzip
import json
from pathlib import Path
import subprocess
import tempfile


EXCLUSION = "GG_COV_EXCL_BRANCH"


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--source-dir", required=True, type=Path)
    return parser.parse_args()


def run(
    command: list[str], *, cwd: Path | None = None, quiet: bool = False
) -> None:
    subprocess.run(
        command,
        cwd=cwd,
        check=True,
        stdout=subprocess.DEVNULL if quiet else None,
    )


def main() -> int:
    options = arguments()
    build_dir = options.build_dir.resolve()
    source_dir = options.source_dir.resolve()
    if not build_dir.is_dir() or not (source_dir / "src").is_dir():
        raise SystemExit("coverage paths do not identify a configured gg build")

    for profile in build_dir.rglob("*.gcda"):
        profile.unlink()
    run(["ctest", "--test-dir", str(build_dir), "--output-on-failure"])

    notes = sorted((build_dir / "CMakeFiles/gg_impl.dir/src").glob("*.gcno"))
    notes += sorted((build_dir / "CMakeFiles/gg_lib.dir/src").glob("*.gcno"))
    notes += sorted((build_dir / "CMakeFiles/gg_cli_lib.dir/src").glob("*.gcno"))
    notes += sorted((build_dir / "CMakeFiles/gg_cli.dir/src").glob("*.gcno"))
    if not notes:
        raise SystemExit("no coverage notes found; configure with GG_COVERAGE=ON")

    source_root = (source_dir / "src").resolve()
    reports: dict[Path, dict[int, dict]] = {}
    with tempfile.TemporaryDirectory(prefix="gg-coverage-") as temporary:
        report_dir = Path(temporary)
        run(
            [
                "gcov",
                "--json-format",
                "--branch-counts",
                "--branch-probabilities",
                *map(str, notes),
            ],
            cwd=report_dir,
            quiet=True,
        )
        for report in report_dir.glob("*.gcov.json.gz"):
            with gzip.open(report, "rt", encoding="utf-8") as stream:
                data = json.load(stream)
            for file_data in data["files"]:
                path = Path(file_data["file"]).resolve()
                if path.parent != source_root or path.suffix != ".cpp":
                    continue
                reports[path] = {
                    line["line_number"]: line for line in file_data["lines"]
                }

    missing_lines: list[str] = []
    missing_branches: list[str] = []
    line_total = line_covered = branch_total = branch_covered = 0
    for path in sorted(reports):
        source_lines = path.read_text(encoding="utf-8").splitlines()
        file_line_total = file_line_covered = 0
        file_branch_total = file_branch_covered = 0
        for number, line in reports[path].items():
            file_line_total += 1
            # GCC can report count zero for a line while explicitly saying that
            # none of its basic blocks are unexecuted. Treat that as covered.
            if line["count"] > 0 or not line["unexecuted_block"]:
                file_line_covered += 1
            else:
                missing_lines.append(f"{path.name}:{number}")

            if EXCLUSION in source_lines[number - 1]:
                continue
            branches = line.get("branches", [])
            for index in range(0, len(branches), 2):
                pair = branches[index : index + 2]
                if len(pair) != 2:
                    continue
                if any(branch.get("throw", False) for branch in pair):
                    continue
                if sum(branch["count"] for branch in pair) == 0:
                    continue
                file_branch_total += 2
                covered = sum(branch["count"] > 0 for branch in pair)
                file_branch_covered += covered
                if covered != 2:
                    missing_branches.append(f"{path.name}:{number}")

        line_total += file_line_total
        line_covered += file_line_covered
        branch_total += file_branch_total
        branch_covered += file_branch_covered
        print(
            f"{path.name}: lines {file_line_covered}/{file_line_total}, "
            f"branches {file_branch_covered}/{file_branch_total}"
        )

    if not reports:
        raise SystemExit("gcov produced no reports for gg sources")
    print(
        f"Total: lines {line_covered}/{line_total} "
        f"({100 * line_covered / line_total:.2f}%), branches "
        f"{branch_covered}/{branch_total} "
        f"({100 * branch_covered / branch_total if branch_total else 100:.2f}%)"
    )
    if missing_lines:
        print("Uncovered lines: " + ", ".join(sorted(set(missing_lines))))
    if missing_branches:
        print("Uncovered branches: " + ", ".join(sorted(set(missing_branches))))
    return 1 if missing_lines or missing_branches else 0


if __name__ == "__main__":
    raise SystemExit(main())
