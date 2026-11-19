"""Unit tests of the comparison and the parsing in differential.py (DU-01 to DU-06).

They run neither strata_cli nor a corpus; only test_compare_error_classes opens an
in-memory SQLite database, to get the exceptions that SQLite really raises.
"""

from __future__ import annotations

import sqlite3
import unittest

import differential
from differential import Cell, Outcome, Statement


def sqlite_rows(*rows: tuple[Cell, ...]) -> tuple[tuple[Cell, ...], ...]:
    return tuple(rows)


class CompareTest(unittest.TestCase):
    def test_compare_reads_cells_by_the_sqlite_type(self) -> None:
        self.assertTrue(differential.cell_matches("7", 7))
        self.assertTrue(differential.cell_matches("-12", -12))
        self.assertFalse(differential.cell_matches("1", 1.0))
        self.assertFalse(differential.cell_matches("1.0", 1))
        self.assertTrue(differential.cell_matches("1.5", 1.5))
        self.assertTrue(differential.cell_matches("1.0000000001", 1.0))
        self.assertFalse(differential.cell_matches("1.1", 1.0))
        self.assertTrue(differential.cell_matches("-0.0", 0.0))
        self.assertTrue(differential.cell_matches("abc", "abc"))
        self.assertFalse(differential.cell_matches("abc", "abd"))
        # A TEXT is compared as text, so two spellings of zero differ.
        self.assertFalse(differential.cell_matches("-0.0", "0.0"))
        # A TEXT that looks like a number is not read as one.
        self.assertTrue(differential.cell_matches("123", "123"))
        self.assertFalse(differential.cell_matches("123", 124))
        self.assertFalse(differential.cell_matches("0", None))

    def test_compare_rows_as_multiset_or_sequence(self) -> None:
        strata = (("1", "a"), ("2", "b"), ("2", "b"))
        shuffled = sqlite_rows((2, "b"), (1, "a"), (2, "b"))
        self.assertTrue(differential.rows_match(strata, shuffled, False))
        self.assertFalse(differential.rows_match(strata, shuffled, True))
        self.assertTrue(
            differential.rows_match(
                strata, sqlite_rows((1, "a"), (2, "b"), (2, "b")), True
            )
        )
        # A different count never matches, and neither does a different repeat.
        self.assertFalse(differential.rows_match(strata, shuffled[:2], False))
        repeated = sqlite_rows((1, "a"), (1, "a"), (2, "b"))
        self.assertFalse(differential.rows_match(strata, repeated, False))
        self.assertTrue(differential.rows_match((), (), True))

    def test_compare_error_classes(self) -> None:
        constraint = differential.outcome_from_cli(
            ["error: constraint violation: duplicate key: t.id=1"]
        )
        self.assertEqual(constraint.error_class, "constraint")
        for line in (
            "error: bind: unknown table: x",
            "error: unsupported: something",
            "error: runtime: division by zero",
            "error: io: could not write",
            "syntax error at column 3: expected FROM, found ';'",
        ):
            outcome = differential.outcome_from_cli([line])
            self.assertEqual((outcome.kind, outcome.error_class), ("error", "other"))

        connection = sqlite3.connect(":memory:", isolation_level=None)
        self.addCleanup(connection.close)
        connection.execute("CREATE TABLE t (id INT PRIMARY KEY)")
        connection.execute("INSERT INTO t VALUES (1)")
        integrity = differential.outcome_from_sqlite(
            connection, "INSERT INTO t VALUES (1)"
        )
        operational = differential.outcome_from_sqlite(
            connection, "SELECT * FROM nosuch"
        )
        programming = differential.outcome_from_sqlite(connection, "SELECT 1; SELECT 2")
        self.assertEqual(integrity.error_class, "constraint")
        self.assertEqual(operational.error_class, "other")
        self.assertEqual(programming.error_class, "other")
        self.assertTrue(differential.agrees(constraint, integrity, False))
        self.assertFalse(differential.agrees(constraint, operational, False))
        other = Outcome("error", error_class="other", message="a")
        self.assertTrue(differential.agrees(other, programming, False))

    def test_cli_output_is_split_at_the_marker(self) -> None:
        marker = differential.MARKER_LINE
        text = f"engine paged\nloaded no tables\n{marker}\nid\tname\n1\tann\n2\tbob\n"
        lines = differential.split_cli_output(text)
        self.assertEqual(lines, ["id\tname", "1\tann", "2\tbob"])
        rows = differential.outcome_from_cli(lines)
        self.assertEqual(rows.kind, "rows")
        self.assertEqual(rows.columns, ("id", "name"))
        self.assertEqual(rows.rows, (("1", "ann"), ("2", "bob")))
        self.assertEqual(differential.outcome_from_cli(["id"]).rows, ())
        affected = differential.outcome_from_cli(["OK affected=3"])
        self.assertEqual((affected.kind, affected.count), ("affected", 3))
        done = differential.outcome_from_cli(["OK CREATE TABLE t", "note: x"])
        self.assertEqual(done.kind, "done")
        self.assertEqual(differential.outcome_from_cli([]).kind, "done")
        with self.assertRaises(differential.HarnessError):
            differential.split_cli_output("engine paged\nid\n1\n")


class CorpusTest(unittest.TestCase):
    def test_corpus_parsing(self) -> None:
        text = (
            "-- A plain comment; with a semicolon.\n"
            "CREATE TABLE t (id INT PRIMARY KEY, v TEXT);\n"
            "INSERT INTO t VALUES (1, 'a;b'), (2, 'it''s'); SELECT 1 FROM t;\n"
            "-- differs: Division by zero\n"
            "-- order: total\n"
            "SELECT id, v / 0 AS q  -- not a marker\n"
            "  FROM t ORDER BY id;\n"
            "SELECT id FROM t"
        )
        statements = differential.parse_corpus(text)
        self.assertEqual(
            [statement.text for statement in statements],
            [
                "CREATE TABLE t (id INT PRIMARY KEY, v TEXT)",
                "INSERT INTO t VALUES (1, 'a;b'), (2, 'it''s')",
                "SELECT 1 FROM t",
                "SELECT id, v / 0 AS q  \n  FROM t ORDER BY id",
                "SELECT id FROM t",
            ],
        )
        marked = statements[3]
        self.assertEqual(marked.differs, "division by zero")
        self.assertTrue(marked.is_total_order)
        self.assertEqual(marked.line, 6)
        # A marker applies to the next statement only.
        for statement in (statements[0], statements[1], statements[2], statements[4]):
            self.assertIsNone(statement.differs)
            self.assertFalse(statement.is_total_order)
        for bad in (
            "-- differs: no such area\nSELECT 1 FROM t;",
            "-- order: partial\nSELECT 1 FROM t;",
            "SELECT 1 -- differs: Division by zero\n FROM t;",
            "-- differs: Division by zero\n",
            "-- differs: Division by zero\n-- differs: Limit -1\nSELECT 1 FROM t;",
        ):
            with self.assertRaises(differential.HarnessError, msg=bad):
                differential.parse_corpus(bad)

    def test_marker_enforcement(self) -> None:
        agree_rows = ["id", "1"]
        statement = Statement("SELECT id FROM t", 1)
        marked = Statement("SELECT id FROM t", 1, differs="division by zero")
        same = Outcome("rows", columns=("id",), rows=((1,),))
        other = Outcome("rows", columns=("id",), rows=((2,),))

        self.assertEqual(
            differential.judge(statement, agree_rows, agree_rows, same).kind, "agree"
        )
        unmarked = differential.judge(statement, agree_rows, agree_rows, other)
        self.assertEqual(unmarked.kind, "unexpected")
        self.assertIn("strata answered", unmarked.detail)
        self.assertEqual(
            differential.judge(marked, agree_rows, agree_rows, other).kind, "expected"
        )
        stale = differential.judge(marked, agree_rows, agree_rows, same)
        self.assertEqual(stale.kind, "unexpected")
        self.assertIn("division by zero", stale.detail)
        engines = differential.judge(statement, agree_rows, ["id", "2"], same)
        self.assertEqual(engines.kind, "unexpected")
        self.assertIn("flatfile printed", engines.detail)

        report = differential.Report(4, 1, ("unexpected: line 1: x\n  y",))
        self.assertEqual(
            report.summary(),
            "differential: 4 statements, 1 expected differences, 1 unexpected",
        )


if __name__ == "__main__":
    unittest.main()
