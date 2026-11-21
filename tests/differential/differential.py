"""Compare Strata with SQLite on a statement corpus (ADR-0047).

usage: python3 -I tests/differential/differential.py --cli <strata_cli>
                                                       [--corpus <file>]

Every statement of the corpus runs on three systems: the `sqlite3` module (an
in-memory database with foreign keys on), and `strata_cli` with the `flatfile`
engine and with the `paged` engine. Each `strata_cli` statement is its own
process over a directory that the earlier statements wrote, so a statement
reads what the previous one stored.

A statement that is marked `-- differs: <area>` must differ from SQLite, and
every other statement must agree with it; the two engines must agree with each
other on every statement. The last line printed is
`differential: <n> statements, <e> expected differences, <u> unexpected`, and
the exit status is 1 when <u> is not 0.

Needs Python 3.10 or later and no third-party package.
"""

from __future__ import annotations

import argparse
import dataclasses
import math
import pathlib
import re
import sqlite3
import subprocess
import sys
import tempfile
from collections.abc import Sequence

# A cell as SQLite returns it, or as the text of strata_cli prints it.
Cell = int | float | str | None

# The command that makes strata_cli print one known line, so that the output of
# the statement can be told from the lines that `.open` prints before it.
MARKER_COMMAND = ".schema strata_marker"
MARKER_LINE = "error: no such table: strata_marker"

# The areas of Design Document 2.3 in which Strata differs from SQLite.
AREAS = frozenset(
    {
        "aggregates over no rows",
        "division by zero",
        "int overflow",
        "text with a number",
        "cast of text",
        "round and substr ranges",
        "substr negative start",
        "conditions",
        "order by and group by rules",
        "limit -1",
        "int against double",
        "double text",
        "drop of a referenced table",
        "add column default",
    }
)

INT_TEXT = re.compile(r"-?[0-9]+")
DOUBLE_TEXT = re.compile(r"-?[0-9]+\.[0-9]+")
RELATIVE_TOLERANCE = 1e-9
ABSOLUTE_TOLERANCE = 1e-12
PROCESS_TIMEOUT_SECONDS = 120


class HarnessError(Exception):
    """A problem in the harness or the corpus, not a difference between systems."""


@dataclasses.dataclass(frozen=True)
class Statement:
    """One statement of the corpus, without its terminating semicolon."""

    text: str
    line: int
    differs: str | None = None
    is_total_order: bool = False


@dataclasses.dataclass(frozen=True)
class Outcome:
    """What one system answered to one statement."""

    kind: str  # "rows", "affected", "done" or "error"
    columns: tuple[str, ...] = ()
    rows: tuple[tuple[Cell, ...], ...] = ()
    count: int = 0
    error_class: str = ""  # "constraint" or "other", for kind "error"
    message: str = ""


@dataclasses.dataclass(frozen=True)
class Verdict:
    """The judgment on one statement: "agree", "expected" or "unexpected"."""

    kind: str
    detail: str = ""


@dataclasses.dataclass(frozen=True)
class Report:
    statement_count: int
    expected_count: int
    problems: tuple[str, ...]

    def summary(self) -> str:
        return (
            f"differential: {self.statement_count} statements, "
            f"{self.expected_count} expected differences, "
            f"{len(self.problems)} unexpected"
        )


# ---------------------------------------------------------------------------
# The corpus.
# ---------------------------------------------------------------------------


def parse_marker(comment: str) -> tuple[str | None, bool]:
    """Reads a comment line: its `differs` area and its `order: total` flag.

    A comment that is neither marker gives (None, False). An unknown area or
    another order raises HarnessError.
    """
    text = comment.strip()
    if text.lower().startswith("differs:"):
        area = text[len("differs:") :].strip().lower()
        if area not in AREAS:
            raise HarnessError(f"unknown area in the marker: {text}")
        return area, False
    if text.lower().startswith("order:"):
        if text[len("order:") :].strip().lower() != "total":
            raise HarnessError(f"unknown order in the marker: {text}")
        return None, True
    return None, False


def parse_corpus(text: str) -> list[Statement]:
    """Splits the corpus text into statements, each with its markers.

    A statement ends at a semicolon outside a string literal. A marker comment
    applies to the next statement only, and a marker inside a statement is an
    error.
    """
    statements: list[Statement] = []
    pending_area: str | None = None
    pending_total = False
    buffer: list[str] = []
    start_line = 0
    line = 1
    is_in_string = False
    index = 0
    while index < len(text):
        char = text[index]
        if char == "\n":
            line += 1
        if is_in_string:
            buffer.append(char)
            if char == "'":
                if text.startswith("''", index):
                    buffer.append("'")
                    index += 1
                else:
                    is_in_string = False
        elif text.startswith("--", index):
            end = text.find("\n", index)
            end = len(text) if end == -1 else end
            area, is_total = parse_marker(text[index + 2 : end])
            if area is not None or is_total:
                if "".join(buffer).strip():
                    raise HarnessError(f"line {line}: a marker inside a statement")
                if area is not None:
                    if pending_area is not None:
                        raise HarnessError(f"line {line}: two differs markers")
                    pending_area = area
                if is_total:
                    pending_total = True
            index = end - 1
        elif char == ";":
            statement_text = "".join(buffer).strip()
            if statement_text:
                statements.append(
                    Statement(statement_text, start_line, pending_area, pending_total)
                )
                pending_area, pending_total = None, False
            buffer = []
        else:
            if char == "'":
                is_in_string = True
            if not buffer and not char.isspace():
                start_line = line
            if buffer or not char.isspace():
                buffer.append(char)
        index += 1
    leftover = "".join(buffer).strip()
    if leftover:
        statements.append(Statement(leftover, start_line, pending_area, pending_total))
    elif pending_area is not None or pending_total:
        raise HarnessError("a marker at the end of the corpus has no statement")
    return statements


# ---------------------------------------------------------------------------
# What the systems answer.
# ---------------------------------------------------------------------------


def split_cli_output(output: str) -> list[str]:
    """The lines that strata_cli printed after the marker line."""
    lines = output.splitlines()
    for index, line in enumerate(lines):
        if line == MARKER_LINE:
            return lines[index + 1 :]
    raise HarnessError("the marker line is missing from the output of strata_cli")


def outcome_from_cli(lines: Sequence[str]) -> Outcome:
    """Reads the output of one statement as printed by strata_cli."""
    if not lines:
        return Outcome("done")
    first = lines[0]
    if first.startswith("error: "):
        is_constraint = first.startswith("error: constraint violation:")
        return Outcome(
            "error",
            error_class="constraint" if is_constraint else "other",
            message=first,
        )
    if first.startswith("syntax error"):
        return Outcome("error", error_class="other", message=first)
    if first.startswith("OK affected="):
        return Outcome("affected", count=int(first[len("OK affected=") :]))
    if first.startswith("OK "):
        return Outcome("done", message=first)
    rows = tuple(tuple(line.split("\t")) for line in lines[1:])
    return Outcome("rows", columns=tuple(first.split("\t")), rows=rows)


def outcome_from_sqlite(connection: sqlite3.Connection, text: str) -> Outcome:
    """Runs one statement on SQLite and reads its answer."""
    try:
        cursor = connection.execute(text)
        if cursor.description is not None:
            columns = tuple(item[0] for item in cursor.description)
            rows = tuple(tuple(row) for row in cursor.fetchall())
            return Outcome("rows", columns=columns, rows=rows)
    except sqlite3.IntegrityError as error:
        return Outcome("error", error_class="constraint", message=str(error))
    except sqlite3.Error as error:
        return Outcome("error", error_class="other", message=str(error))
    words = text.split(None, 1)
    keyword = words[0].upper() if words else ""
    if keyword in ("INSERT", "UPDATE", "DELETE"):
        return Outcome("affected", count=cursor.rowcount)
    return Outcome("done")


# ---------------------------------------------------------------------------
# Comparison.
# ---------------------------------------------------------------------------


def cell_matches(cell: str, expected: Cell) -> bool:
    """True when the text `cell` is the SQLite value `expected`.

    The SQLite type decides how the text is read: an INT matches exactly, a
    DOUBLE to a relative 1e-9, and a TEXT exactly. NULL never matches, because
    Strata has no NULL.
    """
    if expected is None:
        return False
    if isinstance(expected, int):
        return INT_TEXT.fullmatch(cell) is not None and int(cell) == expected
    if isinstance(expected, float):
        return DOUBLE_TEXT.fullmatch(cell) is not None and math.isclose(
            float(cell),
            expected,
            rel_tol=RELATIVE_TOLERANCE,
            abs_tol=ABSOLUTE_TOLERANCE,
        )
    return cell == expected


def row_matches(strata_row: Sequence[Cell], sqlite_row: Sequence[Cell]) -> bool:
    if len(strata_row) != len(sqlite_row):
        return False
    for cell, expected in zip(strata_row, sqlite_row):
        if not isinstance(cell, str) or not cell_matches(cell, expected):
            return False
    return True


def rows_match(
    strata_rows: Sequence[Sequence[Cell]],
    sqlite_rows: Sequence[Sequence[Cell]],
    is_total_order: bool,
) -> bool:
    """Rows as a sequence when the order is total, else as a multiset."""
    if len(strata_rows) != len(sqlite_rows):
        return False
    if is_total_order:
        for strata_row, sqlite_row in zip(strata_rows, sqlite_rows):
            if not row_matches(strata_row, sqlite_row):
                return False
        return True
    unmatched = list(sqlite_rows)
    for strata_row in strata_rows:
        for position, candidate in enumerate(unmatched):
            if row_matches(strata_row, candidate):
                del unmatched[position]
                break
        else:
            return False
    return True


def agrees(strata: Outcome, sqlite: Outcome, is_total_order: bool) -> bool:
    """True when Strata's answer is SQLite's, by the rules of ADR-0047."""
    if strata.kind != sqlite.kind:
        return False
    if strata.kind == "error":
        return strata.error_class == sqlite.error_class
    if strata.kind == "affected":
        return strata.count == sqlite.count
    if strata.kind == "rows":
        strata_names = [name.lower() for name in strata.columns]
        sqlite_names = [name.lower() for name in sqlite.columns]
        return strata_names == sqlite_names and rows_match(
            strata.rows, sqlite.rows, is_total_order
        )
    return True


def engines_agree(
    flatfile_lines: Sequence[str], paged_lines: Sequence[str], is_total_order: bool
) -> bool:
    """True when both engines printed the same, rows as a multiset unless total."""
    if is_total_order or not flatfile_lines or not paged_lines:
        return list(flatfile_lines) == list(paged_lines)
    return (
        flatfile_lines[0] == paged_lines[0]
        and len(flatfile_lines) == len(paged_lines)
        and sorted(flatfile_lines[1:]) == sorted(paged_lines[1:])
    )


def describe(outcome: Outcome) -> str:
    if outcome.kind == "rows":
        return f"rows {list(outcome.columns)} {list(outcome.rows)[:6]}"
    if outcome.kind == "affected":
        return f"affected={outcome.count}"
    if outcome.kind == "error":
        return f"{outcome.error_class} error: {outcome.message}"
    return f"done {outcome.message}".strip()


def judge(
    statement: Statement,
    flatfile_lines: Sequence[str],
    paged_lines: Sequence[str],
    sqlite: Outcome,
) -> Verdict:
    """The verdict on one statement, from the three answers."""
    if not engines_agree(flatfile_lines, paged_lines, statement.is_total_order):
        return Verdict(
            "unexpected",
            f"flatfile printed {list(flatfile_lines)[:6]} "
            f"and paged printed {list(paged_lines)[:6]}",
        )
    strata = outcome_from_cli(flatfile_lines)
    is_agreement = agrees(strata, sqlite, statement.is_total_order)
    if statement.differs is None:
        if is_agreement:
            return Verdict("agree")
        return Verdict(
            "unexpected",
            f"strata answered {describe(strata)} and sqlite {describe(sqlite)}",
        )
    if is_agreement:
        return Verdict(
            "unexpected",
            f"marked as differing ({statement.differs}) but both answered "
            f"{describe(strata)}",
        )
    return Verdict("expected", statement.differs)


# ---------------------------------------------------------------------------
# Running.
# ---------------------------------------------------------------------------


def run_strata(
    cli: pathlib.Path, engine: str, directory: pathlib.Path, text: str
) -> list[str]:
    """One statement in its own strata_cli process over `directory`."""
    commands = [".engine paged"] if engine == "paged" else []
    commands += [f".open {directory}", MARKER_COMMAND, f"{text};"]
    completed = subprocess.run(
        [str(cli)],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        timeout=PROCESS_TIMEOUT_SECONDS,
        check=False,
    )
    if completed.returncode != 0:
        raise HarnessError(
            f"strata_cli exited with status {completed.returncode} on: {text}"
        )
    return split_cli_output(completed.stdout)


def run_corpus(cli: pathlib.Path, statements: Sequence[Statement]) -> Report:
    """Runs every statement on the three systems and collects the verdicts."""
    expected_count = 0
    problems: list[str] = []
    with tempfile.TemporaryDirectory(prefix="strata-differential-") as work:
        flatfile_directory = pathlib.Path(work) / "flatfile"
        paged_directory = pathlib.Path(work) / "paged"
        connection = sqlite3.connect(":memory:", isolation_level=None)
        try:
            connection.execute("PRAGMA foreign_keys = ON")
            for statement in statements:
                flatfile_lines = run_strata(
                    cli, "flatfile", flatfile_directory, statement.text
                )
                paged_lines = run_strata(cli, "paged", paged_directory, statement.text)
                sqlite = outcome_from_sqlite(connection, statement.text)
                verdict = judge(statement, flatfile_lines, paged_lines, sqlite)
                if verdict.kind == "expected":
                    expected_count += 1
                elif verdict.kind == "unexpected":
                    first_line = statement.text.splitlines()[0]
                    problems.append(
                        f"unexpected: line {statement.line}: {first_line}\n"
                        f"  {verdict.detail}"
                    )
        finally:
            connection.close()
    return Report(len(statements), expected_count, tuple(problems))


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cli", type=pathlib.Path, required=True)
    parser.add_argument(
        "--corpus",
        type=pathlib.Path,
        default=pathlib.Path(__file__).with_name("corpus.sql"),
    )
    arguments = parser.parse_args(argv)
    try:
        statements = parse_corpus(arguments.corpus.read_text(encoding="utf-8"))
        report = run_corpus(arguments.cli, statements)
    except (HarnessError, OSError, subprocess.SubprocessError) as error:
        print(f"harness error: {error}", file=sys.stderr)
        return 2
    for problem in report.problems:
        print(problem)
    print(report.summary())
    return 1 if report.problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
