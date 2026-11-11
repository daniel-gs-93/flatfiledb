# Decision Record

This document records the design decisions of flatfiledb, a database driven from a Command-Line Interface (CLI), as Architecture Decision Records (ADR). An entry has up to five parts: the Decision, the Rationale, the Cost, the Current state (what can be observed in the system today) and the Rejected alternatives. The design is in [DesignDocument.md](DesignDocument.md) and the tests are in [TestDocument.md](TestDocument.md).

## Decisions

### ADR-0001: The engine port sits at the logical plan layer

- **Decision:** `IEngine::Execute(const PlanNode&)` takes the logical plan from the Binder. Each engine runs it with its own executor.
- **Rationale:** The project compares whole engines. With the port at the storage layer (`scan`, `lookup`, `insert`), one shared executor would sit above every engine and hide differences in operators and index choices.
- **Cost:** Each engine, flatfile included, needs its own plan executor. In return the engines can be compared on execution as well as storage.
- **Rejected:** A storage-layer port, for the reason above.

### ADR-0002: `DELETE` of a referenced row uses RESTRICT

- **Decision:** A row referenced by a foreign key of another table cannot be deleted. A `DELETE` that selects such a row returns `kConstraintViolation` and deletes nothing.
- **Rationale:** Cascading delete needs its own implementation in every engine, makes the Write-Ahead Log (WAL) record a chain of changes and multiplies the cross-engine test cases.
- **Cost:** A user has to delete the child rows first. In return every engine follows one rule.
- **Rejected:** CASCADE, for the reasons above; SET NULL, because the system has no NULL (ADR-0007).

### ADR-0003: The abstract syntax tree belongs to the Domain, and parsing stays in the Adapter

- **Decision:** The Abstract Syntax Tree (AST) types `Statement` and `Expr` live in `domain/ast.h`. `SqlParser` (Adapter) produces the AST, `Binder` (Domain) consumes it, and `QueryService::Execute` takes a `Statement`.
- **Rationale:** The Application operation is "execute one statement", so its input needs a structured form of Structured Query Language (SQL). The Binder is a pure function and can be tested in the Domain without an engine or files.
- **Cost:** The Domain holds SQL syntax structures. In return the Binder and the Application do not depend on text parsing.
- **Rejected:** Keeping the AST in the Adapter (the Application would depend on the parser); fixed query and update operations (they cannot accept every combination the syntax table allows).

### ADR-0004: flatfile loads by schema and serializes `Row` by schema on write

- **Decision:** `CsvRepository::Load` reads `catalog.csv` and parses each table's Comma-Separated Values (CSV) file into `Row` values by the `TableSchema` stored there (rules in Design Document 2.5). Two operations write: `Commit` replaces several files in one staged step (ADR-0008), and `AppendRows` appends rows. Both serialize `Row` by `TableSchema`. Rows that fail to parse are recorded by line number in `LoadedTable::rejected_lines` and reported by `.open`, because the next rewrite drops them. Loading does not check foreign keys, so those rules live only in `TableSchema::foreign_keys` (ADR-0006).
- **Rationale:** Per-column write methods cannot cover general `INSERT`, `UPDATE` and `DELETE`, and a table created with Data Definition Language (DDL) has no entity class. Dropping rows by foreign key at load time would define the rules twice and lose data silently.
- **Cost:** Orphan rows are kept and unparsable rows are not written back. In return the file equals the in-memory content, and the schema is the only source of the table definition.
- **Current state:** `data/users.csv` has 10004 lines including the header and loads 10000 rows. Line 12 `1s0,scottbates,Port Lisa`, line 17 `15,donaldfisher;Juliefort` and line 18 `15,donaldfisher,Juliefort,Aaron` cannot be loaded. `data/posts.csv` and `data/engagements.csv` have no rejected rows and no orphan foreign key. `.open` prints the number of rejected rows and the first 10 line numbers (Design Document 3.5), here `users=10000 (rejected 3)` on the `loaded` line and `users.csv: rejected lines 12, 17, 18` below it.
- **Rejected:** One write method per updatable column (each new column needs one); filtering by foreign key at load time (the rationale above); keeping unparsable rows on write-back (it needs a map from file lines to in-memory rows).

### ADR-0005: Engine capability differences are expressed as `kUnsupported`

- **Decision:** When an engine meets a plan or data it cannot execute, such as a flatfile TEXT value that contains a newline, it returns `ExecutionError{kUnsupported, <description>}` instead of terminating. The SQL front end never branches on the engine. The Binder returns its own `kUnsupported` only for features no engine supports, such as `UPDATE` of a primary key column.
- **Rationale:** flatfile is the baseline, and later engines can run more kinds of plans. The front end is shared, so capability differences surface in the response of the port.
- **Cost:** An unsupported statement is found only when it executes. In return the front end knows nothing about any engine.
- **Current state:** flatfile returns `kUnsupported` only for TEXT that contains a newline.
- **Rejected:** Checking capability in the front end (it would need to know the engine kinds); a capability query on the port (with one engine there is nothing to query).

### ADR-0006: Referential integrity rules are declared in the schema and enforced by each engine

- **Decision:** A foreign key is declared with `REFERENCES` in `CREATE TABLE` and stored in `TableSchema::foreign_keys` (column position, referenced table, referenced column position). A referenced column must be a primary key or `UNIQUE` (ADR-0013). Updating a referenced column always cascades to the referencing rows, and deleting a referenced row is always rejected (ADR-0002). Every engine enforces these rules (rule table in Design Document 2.5).
- **Rationale:** Every engine needs the same behavior, and rules kept as data can be stored by any engine that persists the catalog. A primary key cannot be updated, so a cascade happens only for a referenced `UNIQUE` column such as `users.username`.
- **Cost:** Each engine implements the checks itself, but the rules are data, not code.
- **Current state:** The 4000 distinct user names in `data/posts.csv` all exist in `data/users.csv`, and the 10000 `username` values in `data/users.csv` are distinct, so `UNIQUE` can be declared. Loading checks neither `UNIQUE` nor foreign keys. When the data contains duplicate `UNIQUE` values, a cascade rewrites every referencing row that has the same value.
- **Rejected:** Hard-coding the rules in a column handler (one handler per rule); a shared enforcement function in the Domain (it needs engine data and breaks the port boundary); an update action on foreign keys (CASCADE is the only action that can occur).

### ADR-0007: SQL semantic scope

- **Decision:** There is no NULL, and `NULL` is a reserved word that produces a syntax error. Keywords and identifiers are case-insensitive. The `WHERE` of `UPDATE` and `DELETE` accepts any `kBool` expression. An `ORDER BY` key is resolved as an integer position, an alias or a structurally identical expression, in that order, and must correspond to an item of the SELECT list. `COUNT` and `SUM` return zero on an empty set.

  `INT` is 64-bit, and `kRuntime` is returned with the message `numeric overflow` when INT overflows or a DOUBLE result is not finite. Expressions have static types, and `kBool` cannot appear in the SELECT list, in an assignment or in `DEFAULT`. A minus sign in operand position followed immediately by a numeric literal forms a negative literal, and otherwise parses as `0 - <operand>`.

  Column types accept aliases: `INTEGER` and `BIGINT` map to INT, `REAL` and `FLOAT` to DOUBLE, and `VARCHAR(n)` and `CHAR(n)` to TEXT with no length check. `NOT NULL` is accepted and has no effect. `INSERT` may omit a column that has a `DEFAULT` or is auto-increment, and `DEFAULT` is a constant expression evaluated at bind time.
- **Rationale:** Without NULL, comparison, aggregation, sorting and indexing need no three-valued logic. The header of `data/engagements.csv` is `postid` while queries write `postId`, so identifiers have to be case-insensitive, as in SQLite. Resolving `ORDER BY` against the SELECT list keeps sorting above `ProjectNode`, so a plan has one structure. Signed integer overflow is undefined behavior in C++, and a non-finite DOUBLE prints as `inf` or `nan` and compares inconsistently, so both become errors. Static types let the Binder reject `WHERE id`. Every value is already non-NULL, so accepting the `NOT NULL` of imported DDL is correct.
- **Cost:** An empty-set `SUM` returns zero where standard SQL returns NULL, `ORDER BY` cannot reference a column outside the SELECT list, `-x` overflows when `x` is the minimum `int64_t`, and the length of `VARCHAR(n)` is not checked. In return the implementation and the test scope stay smaller.
- **Rejected:** Supporting NULL (three-valued logic touches every operator); a `WHERE` that accepts only `id = <constant>` (it deviates from standard SQL); wrapping around on overflow (the result is wrong and nothing reports it); checking `VARCHAR(n)` length (the catalog would record one more length and every `INSERT` and `UPDATE` would gain a check).

### ADR-0008: A multi-file write uses a staged commit to be all-or-nothing

- **Decision:** The write set of `Commit(CommitSet)` holds `catalog.csv` when it changes, the tables whose content changed and the tables to delete. `Commit` checks the write set and writes each changed file to `<file name>.temp`. When every write has succeeded, it renames each original file to `.orig` and each `.temp` file to the original name, one file at a time (a deleted table gets only the first rename), then removes the `.orig` files. If a step fails, files that already have an `.orig` are restored from it, leftover `.temp` files and newly created files are removed, and the engine returns `kIo`. The in-memory tables are replaced only after `Commit` succeeds (flow in Design Document 3.3).
- **Rationale:** A single `UPDATE users SET username = ...` rewrites three files (users, posts and engagements), and a partial write would leave the foreign key data inconsistent at the next load. `CREATE TABLE`, `ALTER TABLE` and `DROP TABLE` change `catalog.csv` and a data file together. Written separately, they can leave a catalog entry without its file, and the database then fails to open.
- **Cost:** Each commit costs two more renames and one delete. In return every file keeps its old content when a step fails.
- **Current state:** `fsync` is not called. A process terminated during the renames leaves `.orig` behind. The load path ignores it, the next `Commit` reports a failure, and the user restores the files. If a restoring rename fails, the error names the `.orig` file that holds the original content. `AppendRows` bypasses the staged commit, so a process terminated midway leaves a half-appended row.
- **Rejected:** No rollback (the data-loss path above); a WAL in flatfile (it belongs to an engine with a different persistence format); routing `INSERT` through `Commit` (appending one row would become rewriting the whole file, which distorts the baseline's write cost).

### ADR-0009: The Binder produces plans with rule-based predicate pushdown and no cost estimation

- **Decision:** The Binder splits `WHERE` on `AND` and places each term at the lowest position where all of its columns are available (rules in Design Document 2.4). The join order follows the way `FROM` is written, chained left-deep.
- **Rationale:** With U rows in users and E rows in engagements, `WHERE u.id = 5805` without pushdown filters after the join, so the join first produces about E merged rows. With pushdown only the matching users rows enter it. Both are O(U + E); pushdown saves the merged rows and their memory.
- **Cost:** The plan shape depends on how `WHERE` is written, but no cost model or statistics are needed. Splitting on `AND` also changes short-circuit scope: a later conjunct may overflow on rows that an earlier one has already excluded.
- **Rejected:** No pushdown (temporary memory grows with E); a cost-based optimizer (it needs statistics and cost formulas).

### ADR-0010: SQL parsing uses a recursive-descent parser

- **Decision:** `SqlParser` handles SELECT, INSERT, UPDATE, DELETE, DDL and expressions with lexical analysis and a recursive-descent parser. Syntax outside the syntax table in Design Document 2.3 returns a syntax error.
- **Rationale:** The project aims to accept standard SQL, whose clauses and expressions combine freely, and the port takes a general plan, so the parse result need not map to a fixed operation.
- **Cost:** The parser is larger and needs more tests. In return new syntax is added by adding grammar rules only.
- **Rejected:** Matching fixed statement shapes with regular expressions (it cannot accept every combination, for example different `WHERE` conditions).

### ADR-0011: Equality joins use a hash join

- **Decision:** `FlatFileExecutor` splits the `ON` condition of a `JoinNode` on `AND` and takes one condition of the form "one column of left equals one column of right" as the hash join key, building the hash table from the right input and probing it with the left. The other `ON` conditions and the predicate of the `FilterNode` directly above the join are residual conditions, evaluated before a combined row is written. Without an equality condition the join is a nested loop. The plan is a left-deep chain of `JoinNode`s that each pick their own key, so the number of tables has no limit, and a key may come from any table on the left.
- **Rationale:** With 10000 rows each in users and engagements, a nested loop needs 10^8 comparisons and would overstate the join cost of the baseline, while a hash join is O(n + m). Filtering the residual conditions only after the join would let the number of temporary rows reach n · m. Running `ON u.id < p.id WHERE u.id <= 400` on a copy of `data/` on an Apple M4 produced 3,919,800 rows and used 1.46 GB (measured 2026-11-09).
- **Cost:** The executor merges the join with the filter above it. In return memory is proportional only to the number of output rows, and `ON a = b AND c` is still O(n + m).
- **Rejected:** Always a nested loop (10^8 comparisons overstate the baseline); sort-merge join (it sorts first, O(n log n)); limiting the number of tables in a join (the left-deep plan and the hash join do not depend on the count).

### ADR-0012: The flatfile CSV format and storage limits

- **Decision:** CSV columns follow Request for Comments (RFC) 4180: a value with a comma or a double quote is wrapped in double quotes, and an embedded double quote is doubled. A TEXT value cannot contain `\r` or a newline, and storing one returns `kUnsupported`. INT is 64-bit. `Commit` and `AppendRows` keep the line ending of the file's header line. The header stays as it is while the columns stay the same; `ALTER TABLE ADD COLUMN` and new files take it from the schema.
- **Rationale:** A newline is refused because the loader isolates errors line by line: a record that could span lines would let one unterminated quote swallow every row after it. The CSV files in `data/` use Carriage Return and Line Feed (CRLF, `\r\n`), and writing `\n` would mix endings.
- **Cost:** TEXT cannot hold a newline. That keeps a format error in one row from spreading.
- **Rejected:** Quoted fields that span lines (one unterminated quote would swallow the rest of the file); escaping newlines (not standard CSV, so other tools misread it); an error on any comma (users hit it with ordinary sentences).

### ADR-0013: Table definitions live in `catalog.csv` and change through DDL

- **Decision:** The schema lives in `catalog.csv` in the database directory, one row per column (format in Design Document 2.2). `CREATE TABLE`, `ALTER TABLE ADD COLUMN` and `DROP TABLE` are AST and plan nodes in the Domain, validated by the Binder and executed by the engine, which updates the catalog and the data file in one `Commit` (ADR-0008). The catalog, not the files in the directory, decides whether a table exists, and the table name `catalog` is reserved. Constraints are column-level only (`PRIMARY KEY`, `UNIQUE`, `NOT NULL`, `DEFAULT`, `REFERENCES`, `AUTOINCREMENT`), with single-column primary and foreign keys. A referenced column must be a primary key or `UNIQUE`. `ADD COLUMN` must carry `DEFAULT` and cannot carry `PRIMARY KEY`, `UNIQUE`, `REFERENCES` or `AUTOINCREMENT`. `DROP TABLE` is rejected while another table references the table.
- **Rationale:** A CSV header holds only column names, so types and keys need a home elsewhere. Without a catalog, types would be guesses (is `00123` an INT or TEXT?) and a stray file would become a table. Storing the original DDL text, as SQLite does, would put the parser in the load path and make the Infrastructure depend on the Adapter. `ADD COLUMN` takes no constraints because each would mean rechecking every old row.
- **Cost:** The catalog is one more file, table files are named `<table name>.csv` in lowercase, and there are no composite keys or table-level constraints. The schema becomes data, and both engines share one DDL front end.
- **Current state:** `data/` has no `catalog.csv`. After its CSV files are adopted with three `CREATE TABLE` statements (ADR-0014), `.open data` works.
- **Rejected:** A header with types such as `id:INT` (no longer an ordinary CSV, and keys still cannot be expressed); the schema as JavaScript Object Notation (JSON) (needs a parser or a new dependency); inferring types at `.open` (guesses); the original DDL text in the catalog (the Infrastructure would depend on the parser).

### ADR-0014: `CREATE TABLE` adopts an existing CSV

- **Decision:** When `CREATE TABLE` runs and the table's CSV already exists, the statement checks the header and parses the rows with the load rules. If the remaining rows satisfy the declared primary key, `UNIQUE` and foreign keys, the CSV stays untouched, only the catalog is written, and the rows are loaded. A violation or a header mismatch reports an error and creates no table. A 0-byte file counts as 0 rows, and the output states the number of rows adopted.
- **Rationale:** The CSV files in `data/` already exist, and attaching them beats moving them away and importing 10000 rows with `INSERT`. The declared constraints have to hold, because later RESTRICT, cascade and uniqueness checks rest on them. Unparsable rows are rejected under the load rules, so the 3 problem lines of `data/users.csv` do not block adoption.
- **Cost:** Rows rejected at adoption disappear from the file at the next rewrite, as at load (ADR-0004). Existing data needs no conversion.
- **Rejected:** Always reporting "file already exists" (the user must move the file first); overwriting the CSV (data loss); skipping validation (the declared constraints might not hold).

### ADR-0015: The database directory is protected by `flock`

- **Decision:** At `.open`, `DirectoryLock::Acquire` takes an exclusive `flock` (`LOCK_EX | LOCK_NB`) on the dedicated file `.flatfiledb.lock` in the database directory. If another process holds it, `.open` fails and the current connection stays. `OpenedDatabase` holds the lock as a `std::shared_ptr<void>`, and reopening the same directory in the same process reuses it. When the directory is not writable (creating the lock file returns `EACCES` or `EROFS`), no lock is taken.
- **Rationale:** Two processes that `.open` the same directory and then write lose data silently: the later writer rewrites the whole file from its own memory. The lock cannot sit on `catalog.csv`, because the staged commit replaces that file by rename and a lock on the old inode protects nothing. An `flock` lock belongs to each `open()`, so a process locking the same directory twice would block on itself, hence the reuse on reopen. The kernel drops the lock when the process exits or crashes, which a Process Identifier (PID) file cannot do.
- **Cost:** Only one process can open a database at a time, even one that only queries. The lock is advisory, so it cannot stop a text editor, and `flock` behaves differently on network file systems. A second writer now gets an explicit error instead of silently overwriting the first.
- **Current state:** When the lock is held, the output is `error: could not open <directory>: database is locked by another process`.
- **Rejected:** A shared lock for readers (at `.open` it is not known whether the connection will write); a PID file (left behind after a crash); a lock on `catalog.csv` (the rename defeats it); no protection (writes are lost silently).

### ADR-0016: Auto-increment takes the current maximum plus 1

- **Decision:** For a column declared `INTEGER PRIMARY KEY AUTOINCREMENT`, an `INSERT` that omits it takes the column's current maximum plus 1, or 1 for an empty table. The engine fills the value in during `PlanMutation`, because the Binder is a pure function that reads no data and only marks `InsertNode::generated_index`. An explicit value is used as given. At the `int64_t` maximum the engine reports `kRuntime` with the message `numeric overflow`. `RETURNING` is not provided.
- **Rationale:** Taking the maximum needs no extra state, so an `INSERT` stays a single-file append instead of also rewriting `catalog.csv`. A value freed by deleting the largest row is reused, which causes no dangling reference because a referenced row cannot be deleted (ADR-0002).
- **Cost:** An id can be reused after the largest row is deleted, which the `AUTOINCREMENT` of SQLite prevents, and each `INSERT` that omits the id scans the table once, O(n). There is no persistent counter to maintain.
- **Rejected:** Recording the next value in `catalog.csv` (every `INSERT` would become a two-file commit); filling the value in the Binder (it cannot read data).

### ADR-0017: The CLI ends a statement with `;`, and a statement may span lines

- **Decision:** The Command-Line Interface (CLI) accumulates input lines and ends a statement at a `;` outside a string literal. A statement may span lines, a line may hold several statements, and a lone `;` is skipped. If the input ends before the statement does, the output is `error: incomplete statement at end of input`. A command that starts with `.` is recognized only when no statement is in progress, and it takes the whole line. The exception is a line containing only `.cancel`, which discards the accumulated text when a statement is in progress outside a string literal (ADR-0023).
- **Rationale:** A longer `CREATE TABLE` needs line breaks, and `.schema` output (ADR-0018) is multi-line already. A `;` terminator is the convention of sqlite3 and psql, and without one the CLI cannot tell an unfinished statement from a one-line one. A string literal can contain `;` and newlines, so the splitting tracks whether it is inside one.
- **Cost:** A statement without a trailing `;` waits for one instead of running, which is what lets a statement span lines.
- **Rejected:** One statement per line (a long DDL cannot be broken, and `.schema` output could not be run again); a blank line as terminator (it is legal content inside a literal); a trailing `\` for continuation (not an SQL convention, and legal inside a literal).

### ADR-0018: `.schema` prints re-executable `CREATE TABLE` statements

- **Decision:** `.schema [<table>]` prints `CREATE TABLE` statements in catalog order, one column per line, each ending with `;`. The output can be fed back to the CLI and recreates the same schema in an empty database. Types are written as the standard `INT`, `DOUBLE` and `TEXT`, and `NOT NULL` is not printed. `FormatCreateTable` in the Adapter produces the text, and the Application returns only `TableSchema`.
- **Rationale:** Users want to see a table's columns and constraints and to move a schema to another directory, and printing DDL does both with no second format or parser. Referenced tables come first in the catalog, so foreign key targets exist when the output runs again. Without NULL, `NOT NULL` has no effect, so omitting it changes nothing.
- **Cost:** The Adapter holds a second textual form of DDL that has to match the parser's grammar; a round-trip test guards that. The gain is a schema that can be carried to another directory and rebuilt.
- **Rejected:** A column list in table form (it cannot be executed again); DDL text returned by the Application (the text format is presentation); the original DDL text in the catalog (see ADR-0013).

### ADR-0019: Syntax and semantics of operators, functions and `LIMIT`

- **Decision:** The binary operators `*`, `/`, `||` and `LIKE` are added. Precedence from lowest to highest is `OR`, `AND`, comparison and `LIKE`, `+ -`, `* /`, `||`, then unary minus and parentheses, and `LIKE` does not chain. `/` between two INT values truncates toward 0, and mixed operands use DOUBLE division. A divisor of 0 reports `kRuntime` `division by zero`, and dividing the minimum INT by `-1` reports `numeric overflow`. `||` accepts only TEXT. A `LIKE` pattern has `%` and `_` and no `ESCAPE`; `_` matches one 8-bit Unicode Transformation Format (UTF-8) character, and American Standard Code for Information Interchange (ASCII) letters match case-insensitively.

  The scalar functions are `LOWER`, `UPPER`, `LENGTH`, `SUBSTR`, `TRIM`, `ABS`, `ROUND` and `CAST(<expr> AS <type>)`, and the aggregates `AVG`, `MIN` and `MAX` are added. Functions and these aggregates are recognized by a name immediately followed by `(` and are not reserved words, while `LIKE`, `LIMIT` and `OFFSET` are. `AVG` returns DOUBLE, and `MIN` and `MAX` return the type of their argument. Without `GROUP BY`, `AVG`, `MIN` and `MAX` over no rows report `kRuntime` `<FN> over no rows`. `ROUND` rounds half away from zero on the binary value the DOUBLE stores, and the result is the DOUBLE nearest to the decimal value.

  A `SELECT` may end with `LIMIT <n> [OFFSET <n>]`, where `<n>` is a non-negative integer literal, and `LimitNode` sits above `SortNode`.
- **Rationale:** The precedence is SQLite's, so an expression brought over from SQLite keeps its meaning. Integer division truncates toward 0 as in C++, PostgreSQL and SQLite, and without NULL a division by 0 can only be an error. Implicit conversion would make `'id=' || 5` depend on a number format, so conversion goes through `CAST`. Without `ESCAPE` every byte of a pattern has one meaning, and the `LIKE` of SQLite is also ASCII case-insensitive. `min`, `max` and `length` are common column names, and recognizing functions by the following `(` leaves existing tables and CSV files alone. `CASE` and date functions are left for later. `ROUND` follows the stored binary value because of cases like `round(2.675, 2)`. On an Apple M4 (measured in C++ on 2026-11-11), `2.675 * 100` rounds to `267.5`, so `std::round` on the product gives `268` (`2.68`). The binary value of 2.675 is `2.67499999999999982236431605997495353221893310546875`, slightly below half, and `std::fma(2.675, 100.0, -267.5)` is `-1.7763568394002505e-14`. The rounding error of the product that `fma` recovers tells "exactly half" from "slightly below half", and sqlite3 3.51.0 returns `2.67` for the same call.
- **Cost:** `LIKE`, `LIMIT` and `OFFSET` can no longer be identifiers, and a negative `SUBSTR` start does not count from the end as it does in SQLite. The rules themselves follow from the binary value and the position, with no exceptions to remember.
- **Current state:** Tests cover the precedence of `||` and `*`, the O(n · m) matching of `LIKE`, the argument checks of functions, each runtime error message (Design Document 2.3) and the parse errors of `LIMIT`. The differential tests against sqlite3 skip the behaviors meant to differ: a negative `SUBSTR` start, `AVG`, `MIN` and `MAX` of an empty set, and division by 0.
- **Rejected:** `CASE` and date functions (new nodes in the Binder and evaluation, and no date type); implicit conversion between TEXT and numbers (the result depends on the format and conflicts with the type rules of `kBind`); `ESCAPE` for `LIKE` (a pattern byte would no longer have one fixed meaning); `LIMIT` accepting expressions (the Binder would need constant evaluation and type checking).

### ADR-0020: A multi-row `INSERT` is all-or-nothing through a single append

- **Decision:** `INSERT ... VALUES (...), (...)` produces one `InsertNode` with several `rows`. `PlanMutation` checks the rows in order and reports the first violation, and a failed statement changes nothing. A duplicate primary key or `UNIQUE` value is checked against the existing rows and the earlier rows of the same statement, a foreign key against the existing tables only. Auto-increment values increase in row order. `IFlatFileRepository::AppendRows` appends all the rows with one write and truncates the file back to its pre-append size on failure.
- **Rationale:** With `AppendRow` called once per row, a failure at row k leaves the first k - 1 rows in the file. One write also opens the file and checks its header once for N rows. Two rows of one statement can carry the same primary key, which a check against the existing rows alone cannot see.
- **Cost:** The primary key and `UNIQUE` check of `PlanInsert` builds a hash set of the existing values, O(n) for n rows, so an N-row check takes O(n + N) instead of O(n · N). A process terminated midway through the write can still leave a partial row (Design Document 2.6).
- **Current state:** For multiple rows, the Binder prefixes its error messages with `row <k>: `.
- **Rejected:** `AppendRow` row by row (not atomic); one `Commit` that rewrites the whole table (it rewrites an O(n) file only to append); `INSERT ... SELECT` (out of scope).

### ADR-0021: `.import` loads an external CSV and infers types from the data

- **Decision:** `.import <file.csv> [<table>] [--pk <column>] [--type <column>=<type>]... [--skip-bad-rows]` imports an external CSV as a new table. `Cli` parses the arguments, and `QueryService::ImportCsv` reads the source through the Domain's `ICsvSource` (implemented in the Infrastructure by `FileCsvSource`), runs the pure function `InferImport` and hands an `ImportTableNode` to the engine, which writes it in one staged commit. A column is inferred as INT or DOUBLE only when writing the parsed value back reproduces the cell exactly, and as TEXT otherwise. The default is strict, so one bad row fails the import; `--skip-bad-rows` skips and reports bad rows, and `--type` sets a type explicitly. A duplicate primary key is always an error. The rules are in Design Document 2.7.
- **Rationale:** Adopting an existing CSV means hand-writing a matching `CREATE TABLE`, the biggest barrier for a new user. A written table is rewritten in the standard format, so if inference treated `007` as INT, the next `UPDATE` would turn `007` into `7` in a column the statement never touched. The exact round-trip rule makes the import change no byte. Load rejects bad rows and reports them because the file already exists; at import the user can still fix the source, so the default is to fail. `data/users.csv` has three bad rows (the `id` on line 12 is `1s0`, and lines 17 and 18 have the wrong column count), so if every row took part in inference, `id` would become TEXT and `WHERE id = 1` would be a type error.
- **Cost:** `users.csv` cannot be imported with one command and no options, `UNIQUE` and foreign keys are not inferred (a CSV that needs them is adopted with `CREATE TABLE`), and quoted fields with newlines are not supported. The import never changes the user's data or drops it silently.
- **Current state:** `data/posts.csv` and `data/engagements.csv` each import 10000 rows with one `.import`. `data/users.csv` by default reports `line 17: expected 3 fields, found 2`, and with `--pk id --type id=INT --skip-bad-rows` it imports 10000 rows and skips 3 rows.
- **Rejected:** Inferring types by majority vote (the threshold is arbitrary, and data is dropped quietly); all columns as TEXT (what the `.import` of SQLite does, and every later numeric comparison would need a `CAST`); modifying the source file (it is the user's data); the CLI reading the file and inferring directly (the CLI only calls Application operations).

### ADR-0022: External modification is detected by file size and modification time

- **Decision:** `CsvRepository` records the size and modification time of `catalog.csv` and of each table file at load; a file that does not exist is recorded too. Before writing, `Commit` and `AppendRows` compare every file they will write or delete against the record. On a difference they return `<file name> changed on disk since it was loaded; run .open again` and write nothing. After its own successful write, the repository updates the record.
- **Rationale:** If a CSV is edited while the CLI is open, the next write statement rewrites the whole table from the old in-memory data and silently overwrites the edit. Measured on 2026-11-11: after one row was appended to `students.csv`, `UPDATE` returned the file to its original 31 lines with no message. `flock` is advisory and cannot stop an editor (ADR-0015). The check costs one `stat` per written file.
- **Cost:** An external change that keeps both size and modification time goes undetected. A rejected write means running `.open` again, which loses nothing because writes are immediate. External edits are no longer overwritten silently.
- **Current state:** After a file is modified following the load, `UPDATE`, `INSERT` or DDL returns `kIo`, the file keeps the external content, and memory is unchanged. A change in size only, or in modification time only, is detected too. The repository's own consecutive writes are not misjudged, and `.open` again recovers the state.
- **Rejected:** Merging at write time (it needs a three-way merge, and the only row identity is the primary key); content hashing (every write would read the whole file once more); a file lock (advisory); silent reload (the user's statement would run on data the user does not know about).

### ADR-0023: CLI input tolerance rules

- **Decision:** `--` to the end of the line starts a comment, and `StatementBuffer` and the lexer ignore `;` and single quotes inside it. U+00A0, U+3000 and U+2000 to U+200A count as whitespace. A full-width semicolon `；` outside string literals and comments prints `error: full-width ';' (U+FF1B) found; use ASCII ';'` and discards the line. Other unexpected characters are described by code point (`U+2019`). While a statement is in progress and the input is outside a string literal, a line containing only `.cancel` discards the accumulated text. When no statement is in progress and a line starts with a command name without the `.`, the CLI prints `error: commands start with '.'; did you mean .<name>` and discards the line. When input stops inside a string literal, the continuation prompt is `'> `. `.clear` clears the screen in interactive mode.
- **Rationale:** Scripts and pasted SQL often carry comments. A Chinese input method in full-width mode produces full-width spaces and semicolons, and curly quotes come from copied documents. These characters are invisible or look like ASCII, so the user sees only that nothing happens. The same happens after `open mydb`: the session stops at the continuation prompt and the only way out is `;` or quitting, because commands are recognized only when no statement is in progress (ADR-0017). `.cancel` is recognized only outside a string literal, because lines inside a literal are data. Ctrl-L does nothing in a terminal without line editing, hence `.clear`.
- **Cost:** `SELECT 1--2` changes meaning from "1 minus -2" to a comment, as in SQLite. A full-width semicolon makes the user retype the line, since accepting it would let input method behavior shape the grammar. A string literal left open is closed only by the closing quote or Ctrl-C. Each of these "no output" cases now gets a message or a way out.
- **Current state:** Each rule has a string-stream test. The command names recognized when the `.` is missing are `open`, `import`, `tables`, `schema`, `help`, `quit`, `exit`, `clear` and `cancel`.
- **Rejected:** Running a command name without `.` directly as a command (a mistyped `open` or similar would run as a command, MySQL style); accepting the full-width semicolon as `;` (it silently extends the grammar); `/* */` comments (they need lexer state across lines); a built-in pager and line editing (they add a dependency).
