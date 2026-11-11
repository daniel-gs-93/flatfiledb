# FlatFileDB

FlatFileDB is a small database that can be run from a terminal. It reads Structured Query Language (SQL) statements through a Command-Line Interface (CLI), parses them and turns each one into a plan, and hands the plan to a storage engine. The parser and the binder, which resolves names and builds the plan, are shared by every engine, so an engine only has to execute plans and decide how data is stored. The first engine, flatfile, is deliberately plain: it is the baseline that later engines will be compared with.

In the flatfile engine a directory is a database. `catalog.csv` records the table definitions, and each table is a Comma-Separated Values (CSV) file. Writes go back to the CSV immediately, and a write that spans several files either completes or changes nothing.

## Documentation

| Document | Contents |
|---|---|
| [`docs/DesignDocument.md`](docs/DesignDocument.md) | Architecture, SQL rules, flows and interfaces |
| [`docs/DecisionRecord.md`](docs/DecisionRecord.md) | The rationale, cost and rejected alternatives of each design decision, written as Architecture Decision Records (`ADR-NNNN`) |
| [`docs/TestDocument.md`](docs/TestDocument.md) | Test matrix |

## Directory Structure

```text
flatfiledb/
├── include/flatfiledb/
│   ├── domain/            # values, syntax tree, plan, Binder, expression evaluation, scalar functions, import inference, engine port
│   ├── application.h      # QueryService: run one statement, import a CSV
│   ├── bootstrap.h        # acquire the directory lock, assemble the engine and QueryService, shared by every entry point
│   ├── sql_parser.h       # SQL parser
│   ├── repl.h, statement_buffer.h
│   │                      # CLI: input loop, commands, statement splitting
│   └── infrastructure.h, flat_file_engine.h, flat_file_executor.h,
│       csv_codec.h, csv_file_source.h, directory_lock.h
│                          # flatfile engine: CSV repository, executor, codec, external CSV reading, directory lock
├── src/                   # the matching implementations; cli.cpp contains only main
├── tests/                 # test harness and the tests of each module
├── docs/                  # design document, decision record and test matrix
├── sample_my_db/          # sample database: course enrollment system (4 tables, including catalog.csv)
└── data/                  # sample data (not in the repository)
```

## Build

The build needs CMake 3.22 or later and a C++20 compiler.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces `build/flatfiledb_cli` and `build/flatfiledb_tests`.

## Quick Start

The CLI reads commands and SQL statements from standard input, line by line, until the input ends. `flatfiledb_cli [<directory> [--parallel]]` opens a database at startup, like typing `.open <directory> [--parallel]` afterwards. Any other arguments print the usage to standard error and exit with status 2.

A statement ends with `;` and can span several lines. One line can hold several statements, a `;` inside a string literal does not end one, and `--` starts a comment that runs to the end of the line. `.open <directory>` has to come before the first SQL statement. If the directory does not exist but its parent does, `.open` creates it and prints `created new database <directory>`.

On a terminal the prompt is `flatfiledb> `, which changes to `       ...> ` inside an unfinished statement and to `         '> ` inside a string literal. Input from a pipe or a file gets no prompt.

| Command | Effect |
|---|---|
| `.open <directory> [--parallel]` | Opens the database directory, creating it if it does not exist |
| `.import <file.csv> [<table>] [--pk <column>] [--type <column>=<type>]... [--skip-bad-rows]` | Imports a CSV into a new table |
| `.tables` | Lists one table per line |
| `.schema [<table>]` | Prints `CREATE TABLE` statements that can be run again |
| `.clear` | Clears the screen; prints nothing unless standard input is a terminal |
| `.cancel` | Abandons an unfinished statement; does nothing inside a string literal |
| `.help` | Prints the command list |
| `.quit`, `.exit` | Exits with status 0 |

A command takes the whole line and only counts as one when no statement is unfinished. Without the leading `.`, as in `open mydb`, the CLI prints `error: commands start with '.'; did you mean .open` and discards the line. A statement ending in a full-width `；` (U+FF1B) is discarded too, with an error that asks for an American Standard Code for Information Interchange (ASCII) semicolon: `error: full-width ';' (U+FF1B) found; use ASCII ';'`.

### Sample Database `sample_my_db`

`sample_my_db/` is a small course enrollment database that ships with the repository. It has four tables: `instructors` (6 rows), `students` (30), `courses` (12) and `enrollments` (144). Each `enrollments` row references a student and a course, and each course references an instructor. `.schema` lists the columns. There is no NULL, so a `score` of 0 means no grade yet, and course capacity and duplicate enrollments are not checked. Every table has an `id`, so a join writes `courses.id`. The other column names are unique across the four tables and can be written bare.

A read-only query can open the database directly (opening only creates `.flatfiledb.lock`, which `.gitignore` excludes):

```bash
CLI="$PWD/build/flatfiledb_cli"
"$CLI" <<'EOF'
.open sample_my_db
.tables
.schema courses
SELECT code, title, credits, capacity FROM courses ORDER BY code LIMIT 4;
SELECT code, COUNT(*)
FROM courses JOIN enrollments ON course_id = courses.id
WHERE semester = '2026-Spring' AND status = 'enrolled'
GROUP BY code ORDER BY 2 DESC, 1 LIMIT 3;
SELECT name, code, score
FROM students
JOIN enrollments ON student_id = students.id
JOIN courses ON course_id = courses.id
WHERE semester = '2025-Fall' AND status = 'completed'
ORDER BY 3 DESC, 1 LIMIT 3;
SELECT id, name, LENGTH(name), UPPER(major) FROM students WHERE name LIKE 'K%';
EOF
```

```text
loaded instructors=6 students=30 courses=12 enrollments=144
instructors
students
courses
enrollments
CREATE TABLE courses (
  id INT PRIMARY KEY AUTOINCREMENT,
  code TEXT UNIQUE,
  title TEXT,
  credits INT,
  dept TEXT,
  capacity INT,
  instructor_id INT REFERENCES instructors(id)
);
code	title	credits	capacity
ART101	Art Appreciation	2	10
CS101	Intro to Programming	3	6
CS201	Data Structures	3	5
CS301	Database Systems	3	5
code	COUNT(*)
HIST101	8
STAT101	8
ECON101	7
name	code	score
Ivy Chen	CS301	97
Wendy Morgan	HIST101	97
Frank Miller	CS301	95
id	name	LENGTH(name)	UPPER(major)
1011	Kevin Anderson	14	CS
```

Writes rewrite the CSV, so this session uses a copy. The foreign key checks reject the `INSERT` and the `DELETE`, and the valid `UPDATE` is written to `enrollments.csv` at once:

```bash
CLI="$PWD/build/flatfiledb_cli"
rm -rf /tmp/sample_my_db_copy && cp -R sample_my_db /tmp/sample_my_db_copy
"$CLI" <<'EOF'
.open /tmp/sample_my_db_copy
SELECT id, student_id, course_id, status, score FROM enrollments WHERE id = 100;
INSERT INTO enrollments (student_id, course_id, semester) VALUES (9999, 1, '2026-Spring');
DELETE FROM students WHERE id = 1001;
UPDATE enrollments SET status = 'completed', score = 90 WHERE id = 100;
SELECT id, student_id, course_id, status, score FROM enrollments WHERE id = 100;
EOF
```

```text
loaded instructors=6 students=30 courses=12 enrollments=144
id	student_id	course_id	status	score
100	1008	3	enrolled	0
error: constraint violation: no matching row: enrollments.student_id=9999 not found in students.id
error: constraint violation: students.id=1001 is referenced by enrollments.student_id
OK affected=1
id	student_id	course_id	status	score
100	1008	3	completed	90
```

To use it interactively, run `"$CLI"` without a pipe, type `.open sample_my_db` at the `flatfiledb> ` prompt and enter statements line by line.

### Starting from an Empty Directory

`.open` creates a directory that does not exist, so no `mkdir` is needed:

```bash
CLI="$PWD/build/flatfiledb_cli"
rm -rf /tmp/mydb
"$CLI" <<'EOF'
.open /tmp/mydb
CREATE TABLE notes (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  body TEXT NOT NULL,
  owner TEXT DEFAULT 'none'
);
INSERT INTO notes (body) VALUES ('Hello, "world"');
ALTER TABLE notes ADD COLUMN pinned INT DEFAULT 0;
SELECT id, body, owner, pinned FROM notes;
EOF
```

Output (columns separated by Tab):

```text
created new database /tmp/mydb
OK CREATE TABLE notes
OK affected=1
OK ALTER TABLE notes
id	body	owner	pinned
1	Hello, "world"	none	0
```

`/tmp/mydb` now holds `catalog.csv` and `notes.csv`, and `.open /tmp/mydb` loads the table again. `--parallel` loads the tables with several threads and gives the same result.

`.open` fails with `error: could not open <directory>: <reason>`, where `<reason>` is `no such directory` when the parent does not exist, `not a directory` when the path is a file, and `cannot create directory` when creation fails. The open database stays as it was.

A statement that spans lines, a `--` comment and `.schema`:

```bash
CLI="$PWD/build/flatfiledb_cli"
"$CLI" <<'EOF'
.open /tmp/mydb
.tables
SELECT id, body  -- two columns
FROM notes
WHERE id = 1;
.schema
EOF
```

```text
loaded notes=1
notes
id	body
1	Hello, "world"
CREATE TABLE notes (
  id INT PRIMARY KEY AUTOINCREMENT,
  body TEXT,
  owner TEXT DEFAULT 'none',
  pinned INT DEFAULT 0
);
```

The `.schema` output doubles as a schema file: feed it to the CLI after `.open` on another empty directory to create the same table there.

### CSV Import

`.import` loads an external CSV into a new table. It infers the column types and the primary key from the data and leaves the source file alone. The example writes a small CSV to `/tmp` first. The comma in `"Eraser, large"` is inside double quotes, so it stays part of the value:

```bash
CLI="$PWD/build/flatfiledb_cli"
rm -rf /tmp/shop_db
cat > /tmp/products.csv <<'EOF'
id,title,price,stock
1,Pencil,0.5,10
2,Notebook,2.25,4
3,"Eraser, large",1.0,0
4,Ruler,1.75,7
EOF
"$CLI" <<'EOF'
.open /tmp/shop_db
.import /tmp/products.csv
.schema products
SELECT id, title, price * stock FROM products ORDER BY 3 DESC;
EOF
```

```text
created new database /tmp/shop_db
OK IMPORT products (imported 4 rows)
CREATE TABLE products (
  id INT PRIMARY KEY,
  title TEXT,
  price DOUBLE,
  stock INT
);
id	title	price * stock
4	Ruler	12.25
2	Notebook	9.0
1	Pencil	5.0
3	Eraser, large	0.0
```

The table name defaults to the file name without its extension, or follows the file name on the command line. It must match `[A-Za-z_][A-Za-z0-9_]*`, cannot be `catalog`, and cannot already exist.

A column is `INT` when every value is an integer in canonical form (`0` or `-?[1-9][0-9]*`), `DOUBLE` when every value is a decimal in canonical form (`0.5`, `1.0`), and `TEXT` otherwise, including when a cell is empty.

Canonical means that the engine's output format reproduces the text. `007`, `+5` and `1.50` are not canonical, and neither is a `10` in a column of decimals.

`--type <column>=<type>` sets a type explicitly (repeatable) and rewrites the values to it. `007` in an `INT` column is stored as `7`, and `1` in a `DOUBLE` column as `1.0`.

The primary key is the column given by `--pk <column>`, or else the `INT` column named `id`. The import fails when neither exists and when key values repeat. It generates no `UNIQUE`, foreign key, `NOT NULL`, `DEFAULT` or `AUTOINCREMENT` constraint.

A bad row has a different column count from the header, unpaired quotes, or a value that does not match its `--type`. By default the import fails and names the line of the first bad row. `--skip-bad-rows` skips the bad rows and lists them.

In the file below, `10x3` on line 4 is not an integer and line 5 lacks a column. The first `.import` stops at line 5. The second stops at line 4 once `order_no` is declared `INT`, and the third skips both:

```bash
CLI="$PWD/build/flatfiledb_cli"
rm -rf /tmp/shop_db2
cat > /tmp/orders.csv <<'EOF'
order_no,customer,amount
1001,Ann,12.5
1002,Bob,7.0
10x3,Cy,9.0
1004,Di
1005,Eve,3.25
EOF
"$CLI" <<'EOF'
.open /tmp/shop_db2
.import /tmp/orders.csv
.import /tmp/orders.csv --pk order_no --type order_no=INT
.import /tmp/orders.csv --pk order_no --type order_no=INT --skip-bad-rows
SELECT * FROM orders ORDER BY order_no;
EOF
```

```text
created new database /tmp/shop_db2
error: bind: line 5: expected 3 fields, found 2
error: bind: line 4: order_no '10x3' is not INT
OK IMPORT orders (imported 3 rows, skipped 2)
skipped line 4: order_no '10x3' is not INT
skipped line 5: expected 3 fields, found 2
order_no	customer	amount
1001	Ann	12.5
1002	Bob	7.0
1005	Eve	3.25
```

The import writes `<table>.csv` and `catalog.csv` in one staged commit, so a failure changes neither. At most 10 skipped rows are listed, then `... and <k> more skipped`.

### Attaching an Existing CSV

When a table's CSV already exists, `CREATE TABLE` adopts the file and its data. The header must match the declared columns (ignoring case), and the data must satisfy the declared primary key, `UNIQUE` and foreign keys.

The statements below attach `users.csv`, `posts.csv` and `engagements.csv` from `data/` (see Data). They work on a copy, because writes rewrite the CSV:

```bash
(
  CLI="$PWD/build/flatfiledb_cli"
  rm -rf /tmp/flatfiledb_demo
  mkdir /tmp/flatfiledb_demo
  cp -R data /tmp/flatfiledb_demo/
  cd /tmp/flatfiledb_demo
  "$CLI" <<'EOF'
.open data
CREATE TABLE users (id INT PRIMARY KEY, username TEXT UNIQUE, location TEXT);
CREATE TABLE posts (
  id INT PRIMARY KEY,
  content TEXT,
  username TEXT REFERENCES users(username),
  views INT
);
CREATE TABLE engagements (
  id INT PRIMARY KEY,
  postId INT REFERENCES posts(id),
  username TEXT REFERENCES users(username),
  type TEXT,
  comment TEXT,
  timestamp INT
);
SELECT username, location FROM users WHERE id = 1;
EOF
)
```

```text
loaded no tables
OK CREATE TABLE users (adopted 10000 rows)
OK CREATE TABLE posts (adopted 10000 rows)
OK CREATE TABLE engagements (adopted 10000 rows)
username	location
knorman	Grahamside
```

From then on `.open /tmp/flatfiledb_demo/data` is enough.

Line 12 of `users.csv` has `1s0` where an integer belongs, and lines 17 and 18 have the wrong column count. The loader rejects all three, so `users=10000` is followed by `(rejected 3)` and a line naming them. A file lists at most 10 line numbers, then `, ...`.

Queries can follow:

```bash
CLI="$PWD/build/flatfiledb_cli"
"$CLI" <<'EOF'
.open /tmp/flatfiledb_demo/data
SELECT id, UPPER(username), LENGTH(username)
FROM users WHERE username LIKE 'kn%' ORDER BY id LIMIT 2;
SELECT COUNT(*), MIN(timestamp), MAX(timestamp), AVG(timestamp) FROM engagements;
EOF
```

```text
loaded users=10000 (rejected 3) posts=10000 engagements=10000
users.csv: rejected lines 12, 17, 18
id	UPPER(username)	LENGTH(username)
1	KNORMAN	7
66	KNIGHTLAURA	11
COUNT(*)	MIN(timestamp)	MAX(timestamp)	AVG(timestamp)
10000	1690823929	1722355167	1706505303.6826
```

## Data

`data/` is not in the repository. The examples read three files from it, with these headers:

| File | Header |
|---|---|
| `data/users.csv` | `id,username,location` |
| `data/posts.csv` | `id,content,username,views` |
| `data/engagements.csv` | `id,postid,username,type,comment,timestamp` |

## Supported SQL

| Statement | Example |
|---|---|
| `CREATE TABLE`, with column constraints `PRIMARY KEY [AUTOINCREMENT]`, `UNIQUE`, `NOT NULL`, `DEFAULT <constant>`, `REFERENCES t(c)`; types `INT`, `DOUBLE`, `TEXT` and the aliases `INTEGER`, `BIGINT`, `REAL`, `FLOAT`, `VARCHAR(n)`, `CHAR(n)` | `CREATE TABLE users (id INT PRIMARY KEY, username TEXT UNIQUE, location TEXT);` |
| `ALTER TABLE ... ADD COLUMN` (a `DEFAULT` is required) | `ALTER TABLE users ADD COLUMN age INT DEFAULT 0;` |
| `DROP TABLE` (rejected while another table references it) | `DROP TABLE notes;` |
| `SELECT`, with `WHERE`, `JOIN ... ON` (any number of tables), `GROUP BY`, `ORDER BY`, `LIMIT ... OFFSET ...`; aggregates `COUNT`, `SUM`, `AVG`, `MIN`, `MAX` | `SELECT e.type, COUNT(*) FROM engagements e JOIN users u ON e.username = u.username WHERE u.location = 'Patriciashire' GROUP BY e.type;` |
| `INSERT`; the column list can leave out a column that has a `DEFAULT` or is auto-increment; `VALUES` can have several rows, and the whole statement is all-or-nothing | `INSERT INTO engagements (id, postId, username, type, comment, timestamp) VALUES (99999999, 1, 'ptaylor', 'like', 'None', 123456);` |
| `UPDATE` | `UPDATE posts SET views = views + 1 WHERE id = 1;` |
| `DELETE` | `DELETE FROM engagements WHERE id = 99999999;` |

Expressions:

| Kind | Supported |
|---|---|
| Operators | `+`, `-`, `*`, `/` (between two `INT` the result is truncated toward 0), `\|\|` (text concatenation), `= <> != < <= > >=`, `LIKE` (`%` and `_`, ASCII case-insensitive), `AND`, `OR` |
| Functions | `LOWER`, `UPPER`, `LENGTH`, `SUBSTR`, `TRIM`, `ABS`, `ROUND`, `CAST(<expression> AS INT\|DOUBLE\|TEXT)` |

Dividing by 0, a `CAST` that cannot convert, and `AVG`, `MIN` or `MAX` over no rows give `error: runtime: <message>`, since there is no NULL to return. Design Document 2.3 has the full rules for operators and functions.

When a table or column does not exist, the message suggests an existing name within edit distance 2, as in `error: bind: unknown column 'nam'; did you mean name?`.

A Data Definition Language (DDL) statement prints `OK <tag>` and a write statement prints `OK affected=<n>`. Errors print `error: <kind>: <message>`. A syntax error prints `syntax error at column <n>: <description>`, or `syntax error at line <l>, column <c>: <description>` when it falls on the second line or later of a multi-line statement. A row that another table's foreign key references cannot be deleted:

```text
DELETE FROM users WHERE id = 5805;
error: constraint violation: users.id=5805 is referenced by posts.username
```

Design Document 2.3 to 2.6 and 3.6 cover syntax, semantics, referential integrity and the error format.

## Limitations of the flatfile Engine

- There is no NULL. `NOT NULL` is accepted but has no effect, and the length in `VARCHAR(n)` is not checked.
- Constraints are column-level: a primary key or foreign key covers one column, and there are no table-level constraints and no `DROP COLUMN`.
- A TEXT value cannot contain a newline. Commas and double quotes are stored with CSV quoting.
- A row that cannot be parsed at load (wrong column count, malformed number, unpaired quotes) is not loaded, and the next `UPDATE`, `DELETE` or `ALTER TABLE` rewrite of the table drops it from the file. `INSERT` only appends a row and leaves those lines in place.
- Load does not check `UNIQUE` and foreign keys, which apply to later statements only.
- Only one process can open a database directory at a time (`.flatfiledb.lock`, `flock`), but the lock does not stop a text editor. If a file's size or modification time changes outside the CLI after loading, a write statement prints `error: io: <file name> changed on disk since it was loaded; run .open again` and writes no file. An edit that leaves both unchanged goes unnoticed.
- Auto-increment takes the current maximum plus 1, so deleting the row with the maximum frees that value for reuse.
- `fsync` is not called, so a write reported as done can still be lost in a power failure.

## Tests

Run all tests, or only those whose name contains a string:

```bash
./build/flatfiledb_tests              # all
./build/flatfiledb_tests test_binder  # tests whose name contains test_binder
```

The tests use a small harness in the repository, so no framework is needed. Without `data/`, the `Environment` tests that read it are skipped, and the tests that use `sample_my_db/` do not depend on it.

## Acknowledgement

The ideas of FlatFileDB are inspired by the flat-file database exercise in CS 6422: Database System Implementation at the Georgia Institute of Technology.
