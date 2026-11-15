#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "flatfiledb/application.h"
#include "flatfiledb/domain/value.h"
#include "flatfiledb/paged/database.h"
#include "flatfiledb/paged/paged_engine.h"
#include "flatfiledb/repl.h"
#include "test_equivalence_generator.h"
#include "test_equivalence_support.h"
#include "test_harness.h"
#include "test_support.h"

// The flat-file engine and the paged engine run the same statements, and the
// results are compared under design document 2.6 (ADR-0044); see
// docs/PagedExecutionTestDocument.md.

namespace flatfiledb::testing {

namespace {

namespace cli = flatfiledb::cli;

using application::QueryService;
using domain::ErrorKind;
using domain::ExecutionError;
using domain::ExecutionResult;
using domain::Row;
using domain::Value;
using paged::ReplacementPolicyKind;

// ----------------------------------------------------------------------------
// Two engines on fresh storage.
// ----------------------------------------------------------------------------

// A flat-file engine over a temporary directory and a paged engine over a
// MemoryPageStore. The directory outlives both services.
struct EnginePair {
  TempDir directory;
  std::unique_ptr<QueryService> flatfile;
  PagedFixture paged;
};

// Reports a failure and returns false when either engine cannot be opened.
bool OpenPair(EnginePair* pair, std::size_t frames,
              ReplacementPolicyKind policy) {
  pair->flatfile = MakeLoadedService(pair->directory.path());
  pair->paged = MakeMemoryPagedFixture(frames, policy);
  return pair->flatfile != nullptr && pair->paged.service != nullptr;
}

const char* PolicyName(ReplacementPolicyKind policy) {
  return policy == ReplacementPolicyKind::kLru ? "LRU" : "2Q";
}

// ----------------------------------------------------------------------------
// What a corpus exercised, so that a corpus that stops reaching a rule fails.
// ----------------------------------------------------------------------------

struct Outcomes {
  std::size_t selects_with_rows = 0;
  std::size_t selects_without_rows = 0;
  std::size_t ordered_limit_selects = 0;
  std::size_t writes_applied = 0;
  std::size_t writes_matching_no_row = 0;
  std::size_t bind_errors = 0;
  std::size_t runtime_errors = 0;
  std::size_t duplicate_keys = 0;
  std::size_t missing_parents = 0;
  std::size_t restricted_deletes = 0;
  std::size_t unsupported_errors = 0;
  std::size_t other_errors = 0;
};

bool Contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

void CountErrorOutcome(const ExecutionError& error, Outcomes* outcomes) {
  switch (error.kind) {
    case ErrorKind::kBind:
      ++outcomes->bind_errors;
      return;
    case ErrorKind::kRuntime:
      ++outcomes->runtime_errors;
      return;
    case ErrorKind::kUnsupported:
      ++outcomes->unsupported_errors;
      return;
    case ErrorKind::kConstraintViolation:
      if (error.message.starts_with("duplicate key")) {
        ++outcomes->duplicate_keys;
      } else if (error.message.starts_with("no matching row")) {
        ++outcomes->missing_parents;
      } else if (Contains(error.message, "is referenced by")) {
        ++outcomes->restricted_deletes;
      } else {
        ++outcomes->other_errors;
      }
      return;
    case ErrorKind::kIo:
      ++outcomes->other_errors;
      return;
  }
}

void CountOutcome(const std::string& sql, const ExecutionResult& result,
                  Outcomes* outcomes) {
  if (const auto* error = std::get_if<ExecutionError>(&result)) {
    CountErrorOutcome(*error, outcomes);
    return;
  }
  if (const auto* rows = std::get_if<domain::ResultSet>(&result)) {
    if (rows->rows.empty()) {
      ++outcomes->selects_without_rows;
    } else {
      ++outcomes->selects_with_rows;
    }
    if (Contains(sql, "ORDER BY") && Contains(sql, "LIMIT")) {
      ++outcomes->ordered_limit_selects;
    }
    return;
  }
  if (const auto* affected = std::get_if<domain::AffectedRows>(&result)) {
    if (affected->count == 0) {
      ++outcomes->writes_matching_no_row;
    } else {
      ++outcomes->writes_applied;
    }
  }
}

// ----------------------------------------------------------------------------
// The comparator itself.
// ----------------------------------------------------------------------------

domain::ResultSet TwoColumnResult(const std::vector<Row>& rows) {
  domain::ResultSet result;
  result.columns = {domain::ColumnDef{"key", domain::DataType::kInt},
                    domain::ColumnDef{"label", domain::DataType::kText}};
  result.rows = rows;
  return result;
}

Row KeyedRow(std::int64_t key, const std::string& label) {
  return Row{Value(key), Value(label)};
}

// A comparator that reports nothing would make every equivalence test pass, so
// each rule of 2.6 is shown to be detected, and each allowed difference shown
// to be accepted.
void CheckComparatorFollowsTheRules() {
  const std::vector<std::size_t> unordered;
  const std::vector<std::size_t> ordered = {0};
  const ExecutionResult base =
      TwoColumnResult({KeyedRow(1, "a"), KeyedRow(1, "b"), KeyedRow(2, "c")});

  // Allowed: another row order without ORDER BY; another order among rows with
  // equal sort keys.
  CHECK(!FindResultDifference(
      base,
      TwoColumnResult({KeyedRow(2, "c"), KeyedRow(1, "b"), KeyedRow(1, "a")}),
      unordered));
  CHECK(!FindResultDifference(
      base,
      TwoColumnResult({KeyedRow(1, "b"), KeyedRow(1, "a"), KeyedRow(2, "c")}),
      ordered));

  // Detected: the order of the sort keys, a changed or missing row, a
  // changed multiplicity, the type of a cell.
  CHECK(FindResultDifference(
      base,
      TwoColumnResult({KeyedRow(1, "a"), KeyedRow(2, "c"), KeyedRow(1, "b")}),
      ordered));
  CHECK(FindResultDifference(
      base,
      TwoColumnResult({KeyedRow(1, "a"), KeyedRow(1, "x"), KeyedRow(2, "c")}),
      ordered));
  CHECK(FindResultDifference(
      base, TwoColumnResult({KeyedRow(1, "a"), KeyedRow(2, "c")}), unordered));
  CHECK(FindResultDifference(
      base,
      TwoColumnResult({KeyedRow(1, "a"), KeyedRow(1, "a"), KeyedRow(2, "c")}),
      unordered));
  domain::ResultSet other_type = TwoColumnResult({KeyedRow(1, "a")});
  other_type.columns[0].type = domain::DataType::kDouble;
  CHECK(FindResultDifference(TwoColumnResult({KeyedRow(1, "a")}), other_type,
                             unordered));
  domain::ResultSet other_name = TwoColumnResult({KeyedRow(1, "a")});
  other_name.columns[1].name = "other";
  CHECK(FindResultDifference(TwoColumnResult({KeyedRow(1, "a")}), other_name,
                             unordered));
  CHECK(FindResultDifference(
      ExecutionResult(
          domain::ResultSet{{domain::ColumnDef{"v", domain::DataType::kDouble}},
                            {Row{Value(2.0)}}}),
      ExecutionResult(
          domain::ResultSet{{domain::ColumnDef{"v", domain::DataType::kDouble}},
                            {Row{Value(std::int64_t{2})}}}),
      unordered));

  // The other result kinds.
  CHECK(!FindResultDifference(domain::AffectedRows{3}, domain::AffectedRows{3},
                              unordered));
  CHECK(FindResultDifference(domain::AffectedRows{3}, domain::AffectedRows{4},
                             unordered));
  CHECK(FindResultDifference(domain::CommandComplete{"CREATE TABLE a"},
                             domain::CommandComplete{"CREATE TABLE b"},
                             unordered));
  CHECK(FindResultDifference(domain::AffectedRows{1},
                             domain::CommandComplete{"CREATE TABLE a"},
                             unordered));
  const ExecutionError error = {ErrorKind::kRuntime, "division by zero"};
  CHECK(!FindResultDifference(error, error, unordered));
  CHECK(FindResultDifference(
      error, ExecutionError{ErrorKind::kBind, "division by zero"}, unordered));
  CHECK(FindResultDifference(
      error, ExecutionError{ErrorKind::kRuntime, "numeric overflow"},
      unordered));
}

// The runner also compares the content of every table, and checks that a
// rejected statement changed none. Each case changes one engine behind the
// runner's back and shows that the next statement reports it.
void CheckRunnerComparesTables() {
  const std::string create = "CREATE TABLE t (id INT PRIMARY KEY, v TEXT)";
  {
    // An INSERT that both engines accept leaves the tables different.
    EnginePair pair;
    if (!OpenPair(&pair, 4, ReplacementPolicyKind::kLru)) {
      return;
    }
    EquivalenceRunner runner(*pair.flatfile, *pair.paged.service, "self-check");
    CHECK(!runner.Run(create));
    CHECK(AsAffectedRows(ExecuteSql(
              *pair.flatfile, "INSERT INTO t VALUES (1, 'x')")) != nullptr);
    const std::optional<std::string> difference =
        runner.Run("INSERT INTO t VALUES (2, 'y')");
    CHECK(difference.has_value() && Contains(*difference, "table t"));
  }
  for (bool is_flatfile : {true, false}) {
    // A rejected statement after one engine changed: the change is reported as
    // made by the rejected statement.
    EnginePair pair;
    if (!OpenPair(&pair, 4, ReplacementPolicyKind::kLru)) {
      return;
    }
    EquivalenceRunner runner(*pair.flatfile, *pair.paged.service, "self-check");
    CHECK(!runner.Run(create));
    CHECK(!runner.Run("INSERT INTO t VALUES (1, 'x')"));
    QueryService* changed =
        is_flatfile ? pair.flatfile.get() : pair.paged.service.get();
    CHECK(AsAffectedRows(ExecuteSql(
              *changed, "INSERT INTO t VALUES (3, 'z')")) != nullptr);
    const std::optional<std::string> difference =
        runner.Run("INSERT INTO t VALUES (1, 'duplicate')");
    CHECK(difference.has_value() &&
          Contains(*difference, "a rejected statement changed table t"));
  }
  {
    // A statement that does not parse, and a LIMIT without ORDER BY, are
    // reported as outside the comparable set rather than compared.
    EnginePair pair;
    if (!OpenPair(&pair, 4, ReplacementPolicyKind::kLru)) {
      return;
    }
    EquivalenceRunner runner(*pair.flatfile, *pair.paged.service, "self-check");
    CHECK(!runner.Run(create));
    CHECK(!runner.Run("INSERT INTO t VALUES (1, 'x'), (2, 'x'), (3, 'y')"));
    CHECK(runner.Run("SELEKT 1").has_value());
    CHECK(runner.Run("SELECT id FROM t LIMIT 1").has_value());
    // Sorted by a key that ties across the cut.
    CHECK(
        runner.Run("SELECT v FROM t ORDER BY v LIMIT 1 OFFSET 1").has_value());
    CHECK(!runner.Run("SELECT v, id FROM t ORDER BY v, id LIMIT 1 OFFSET 1"));
  }
}

// ----------------------------------------------------------------------------
// The fixed corpus.
// ----------------------------------------------------------------------------

// Reads: filters, expressions, functions, LIKE, errors from one source.
const std::vector<std::string>& ReadCorpus() {
  static const std::vector<std::string> corpus = {
      "SELECT * FROM users",
      "SELECT username, location FROM users WHERE location = 'Paris'",
      "SELECT id, username FROM users WHERE rating >= 3.25 AND location <> "
      "'Oslo'",
      "SELECT id FROM posts WHERE views > 3 OR content = ''",
      "SELECT id, views * 2 + 1, views / 2, views - 10, -views FROM posts "
      "WHERE id <= 5",
      "SELECT id, content || '!' FROM posts WHERE content LIKE '%world%'",
      "SELECT id FROM posts WHERE content LIKE 'h_llo%'",
      "SELECT UPPER(username), LOWER(content), LENGTH(content), "
      "SUBSTR(content, 2, 3), TRIM(content) FROM posts",
      "SELECT ABS(rating - 5), ROUND(rating, 1), CAST(rating AS INT), "
      "CAST(id AS TEXT), CAST(rating AS TEXT) FROM users",
      "SELECT id, CAST(content AS INT) FROM posts WHERE id = 1",
      "SELECT id, 1 / (views - 7) FROM posts WHERE id < 6",
      "SELECT id, 10 / views FROM posts WHERE views > 0",
      "SELECT id FROM posts WHERE username = 'nobody'",
      "SELECT id FROM nosuch",
      "SELECT nosuch FROM users",
      "SELECT id FROM users WHERE username > 5",
  };
  return corpus;
}

// ORDER BY with ties, LIMIT and OFFSET at the boundaries, on unique keys.
const std::vector<std::string>& OrderCorpus() {
  static const std::vector<std::string> corpus = {
      "SELECT id, username FROM users ORDER BY username DESC",
      "SELECT id, location FROM users ORDER BY location, id DESC",
      "SELECT id, views FROM posts ORDER BY views DESC, id LIMIT 4",
      "SELECT id FROM posts ORDER BY id LIMIT 3 OFFSET 2",
      "SELECT id FROM posts ORDER BY id LIMIT 0",
      "SELECT id FROM posts ORDER BY id LIMIT 5 OFFSET 20",
      "SELECT id, rating FROM users ORDER BY rating, id",
      "SELECT id, views FROM posts ORDER BY 2 DESC, 1 LIMIT 100",
      "SELECT location, COUNT(*) AS n FROM users GROUP BY location ORDER BY n "
      "DESC, location",
      "SELECT id, content FROM posts ORDER BY content, id",
      "SELECT 1 / views FROM posts ORDER BY 1 LIMIT 1",
  };
  return corpus;
}

// Two-table and three-table joins; hash joins, residual conditions, nested
// loops, INT against DOUBLE, a self join, an empty side, an ambiguous column.
const std::vector<std::string>& JoinCorpus() {
  static const std::vector<std::string> corpus = {
      "SELECT p.id, u.location FROM posts p JOIN users u ON p.username = "
      "u.username",
      "SELECT p.id, e.type FROM posts p JOIN engagements e ON e.postid = p.id "
      "WHERE e.type = 'like'",
      "SELECT u.username, p.id, e.id FROM users u JOIN posts p ON p.username "
      "= u.username JOIN engagements e ON e.postid = p.id",
      "SELECT e.id, e.username, p.username FROM engagements e JOIN posts p ON "
      "e.postid = p.id AND e.username <> p.username",
      "SELECT u.id, v.id FROM users u JOIN users v ON u.rating < v.rating",
      "SELECT u.id, p.id FROM users u JOIN posts p ON u.rating = p.views",
      "SELECT u.id, p.id FROM users u JOIN posts p ON p.views = u.rating",
      "SELECT a.id, b.id FROM posts a JOIN posts b ON a.content = b.content "
      "AND a.id < b.id",
      "SELECT e.id, u.location, p.content FROM engagements e JOIN users u ON "
      "e.username = u.username JOIN posts p ON e.postid = p.id ORDER BY e.id "
      "LIMIT 5",
      "SELECT * FROM users u JOIN posts p ON p.username = u.username",
      "SELECT p.id FROM posts p JOIN users u ON p.username = u.username WHERE "
      "u.location = 'Rome' AND p.views > 2",
      "SELECT p.id FROM posts p JOIN users u ON p.username = u.username WHERE "
      "u.location = 'Nowhere'",
      "SELECT id FROM users JOIN posts ON posts.username = users.username",
      "SELECT e.id, p.id FROM engagements e JOIN posts p ON e.postid = p.id "
      "ORDER BY p.id DESC, e.id LIMIT 6 OFFSET 3",
  };
  return corpus;
}

// Aggregates with and without GROUP BY, an empty input, an overflow.
const std::vector<std::string>& AggregateCorpus() {
  static const std::vector<std::string> corpus = {
      "SELECT COUNT(*), SUM(views), MIN(views), MAX(views), AVG(views) FROM "
      "posts",
      "SELECT MIN(content), MAX(content), MIN(rating), MAX(rating) FROM posts "
      "JOIN users ON posts.username = users.username",
      "SELECT username, COUNT(*), SUM(views) FROM posts GROUP BY username "
      "ORDER BY username",
      "SELECT type, COUNT(*), SUM(weight), MIN(weight), MAX(timestamp) FROM "
      "engagements GROUP BY type ORDER BY type",
      "SELECT COUNT(*), SUM(views) FROM posts WHERE id < 0",
      "SELECT COUNT(*), AVG(views) FROM posts WHERE id < 0",
      "SELECT MIN(views) FROM posts WHERE id < 0",
      "SELECT MAX(content) FROM posts WHERE id < 0",
      "SELECT username, COUNT(*) FROM posts WHERE id < 0 GROUP BY username",
      "SELECT 1, COUNT(*) FROM users",
      "SELECT LENGTH(content), COUNT(*) FROM posts GROUP BY LENGTH(content) "
      "ORDER BY 1",
      "SELECT u.location, COUNT(*) FROM users u JOIN posts p ON p.username = "
      "u.username GROUP BY u.location ORDER BY 2 DESC, 1",
      "SELECT postid, COUNT(*), SUM(timestamp) FROM engagements GROUP BY "
      "postid ORDER BY postid LIMIT 4",
  };
  return corpus;
}

// Writes: INSERT (auto-increment, defaults, every constraint, a multi-row
// statement that fails midway), CREATE TABLE, and the overflow of the
// auto-increment value.
const std::vector<std::string>& InsertCorpus() {
  static const std::vector<std::string> corpus = {
      "INSERT INTO users (username, location) VALUES ('fay', 'Rome')",
      "INSERT INTO users (username, location) VALUES ('gus', 'Lima'), "
      "('hal', 'Oslo'), ('ivy', 'Paris')",
      "INSERT INTO users VALUES (4, 'dup', 'x', 1.0)",
      "INSERT INTO users (username, location) VALUES ('ann', 'x')",
      "INSERT INTO users VALUES (50, 'ann', 'x', 1.0)",
      "INSERT INTO users VALUES (4, 'ann', 'x', 1.0)",
      "INSERT INTO users (username, location) VALUES ('zed', 'x'), ('zed', "
      "'y')",
      "INSERT INTO users VALUES (20, 'jo', 'x', 7)",
      "INSERT INTO users (username) VALUES ('nolocation')",
      "INSERT INTO posts (content, username) VALUES ('new post', 'ann')",
      "INSERT INTO posts (content, username) VALUES ('p', 'nobody')",
      "INSERT INTO posts VALUES (100, 'x', 'ann', 1), (101, 'y', 'ghost', 2)",
      "INSERT INTO posts (content, username, views) VALUES ('x, \"y\" "
      "\xC3\xBCn\xC3\xAF', 'bob', -3), ('', 'cat', 0)",
      "INSERT INTO engagements VALUES (21, 1, 'bob', 'like', 1, 2000)",
      "INSERT INTO engagements VALUES (22, 999, 'bob', 'like', 1.0, 1)",
      "INSERT INTO engagements VALUES (23, 1, 'ghost', 'like', 1.0, 1)",
      "INSERT INTO engagements VALUES (21, 1, 'bob', 'like', 1.0, 1)",
      "INSERT INTO engagements VALUES (24, 1, 'bob', 'like', 1.0, "
      "9223372036854775807)",
      // Several violations in one row: the primary key, then UNIQUE columns in
      // column order, then foreign keys in declaration order.
      "INSERT INTO posts VALUES (1, 'x', 'ghost', 1)",
      "INSERT INTO engagements VALUES (21, 999, 'ghost', 'like', 1.0, 1)",
      "INSERT INTO engagements VALUES (30, 999, 'ghost', 'like', 1.0, 1)",
      "CREATE TABLE twin (id INT PRIMARY KEY, a TEXT UNIQUE, b TEXT UNIQUE)",
      "INSERT INTO twin VALUES (1, 'x', 'y')",
      "INSERT INTO twin VALUES (2, 'x', 'y')",
      "INSERT INTO twin VALUES (2, 'z', 'y')",
      "INSERT INTO twin VALUES (1, 'x', 'y')",
      "CREATE TABLE tagged (id INT PRIMARY KEY, tag TEXT UNIQUE, owner TEXT "
      "REFERENCES users(username))",
      "INSERT INTO tagged VALUES (1, 't', 'ann')",
      "INSERT INTO tagged VALUES (2, 't', 'ghost')",
      "INSERT INTO tagged VALUES (2, 'u', 'ghost')",
      // Two INT values above 2^53 share one DOUBLE value: a join on them has
      // to compare the INT values, not only the hash keys.
      "INSERT INTO engagements VALUES (25, 1, 'bob', 'like', 1.0, "
      "9007199254740992), (26, 1, 'bob', 'like', 1.0, 9007199254740993)",
      "SELECT a.id, b.id FROM engagements a JOIN engagements b ON a.timestamp "
      "= b.timestamp ORDER BY a.id, b.id",
      "INSERT INTO posts (content, username, views) VALUES ('o', 'ann', "
      "9223372036854775807 + 1)",
      // The auto-increment value overflows after the largest INT.
      "INSERT INTO users VALUES (9223372036854775807, 'big', 'x', 1.0)",
      "INSERT INTO users (username, location) VALUES ('after', 'y')",
      "SELECT MAX(id) FROM users",
      "DELETE FROM users WHERE id = 9223372036854775807",
      "INSERT INTO users (username, location) VALUES ('after', 'y')",
      "SELECT id, username FROM users ORDER BY id",
  };
  return corpus;
}

// CREATE TABLE and the auto-increment start value.
const std::vector<std::string>& DefinitionCorpus() {
  static const std::vector<std::string> corpus = {
      "CREATE TABLE jobs (id INT PRIMARY KEY AUTOINCREMENT, body TEXT)",
      "INSERT INTO jobs (body) VALUES ('first')",
      "INSERT INTO jobs VALUES (-5, 'neg')",
      "DELETE FROM jobs WHERE id = 1",
      "INSERT INTO jobs (body) VALUES ('after negative'), ('second')",
      "SELECT * FROM jobs",
      "DELETE FROM jobs",
      "INSERT INTO jobs (body) VALUES ('empty again')",
      "SELECT * FROM jobs",
      "CREATE TABLE notes (id INT PRIMARY KEY, body TEXT DEFAULT 'none', "
      "score DOUBLE DEFAULT 1.5, tag TEXT UNIQUE)",
      "INSERT INTO notes (id, tag) VALUES (1, 't1')",
      "INSERT INTO notes (id, tag) VALUES (2, 't1')",
      "INSERT INTO notes VALUES (3, 'b', 2, 't3')",
      "SELECT id, body, score, tag FROM notes ORDER BY id",
      "CREATE TABLE users (id INT PRIMARY KEY)",
      "CREATE TABLE bad (id INT PRIMARY KEY, a INT, a TEXT)",
      "CREATE TABLE nopk (a INT)",
      "CREATE TABLE refs (id INT PRIMARY KEY, uid INT REFERENCES users(id), "
      "who TEXT REFERENCES users(username))",
      "INSERT INTO refs VALUES (1, 1, 'ann')",
      "INSERT INTO refs VALUES (2, 999, 'ann')",
      "INSERT INTO refs VALUES (3, 8, 'gus')",
      "DELETE FROM users WHERE id = 8",
      "UPDATE users SET username = 'gus2' WHERE id = 8",
      "SELECT id, uid, who FROM refs ORDER BY id",
      "DELETE FROM refs WHERE id > 100",
  };
  return corpus;
}

// UPDATE: assignments, no-ops, uniqueness, foreign keys, the cascade (a swap,
// a chained rename), evaluation errors, and rows that grow until they move to
// another page.
const std::vector<std::string>& UpdateCorpus() {
  static const std::vector<std::string> corpus = {
      "UPDATE users SET rating = rating + 1 WHERE location = 'Paris'",
      "UPDATE users SET location = 'Berlin' WHERE id = 99",
      "UPDATE posts SET views = views + 1",
      "UPDATE posts SET views = views",
      "UPDATE users SET rating = rating * -1 WHERE id = 4",
      "SELECT id, rating FROM users WHERE id = 4",
      "UPDATE users SET rating = 7 WHERE id = 2",
      "UPDATE users SET rating = 'x'",
      "UPDATE users SET id = 77 WHERE id = 1",
      // Two columns exchanged; the changed username cascades.
      "UPDATE users SET location = username, username = location WHERE id = 5",
      "SELECT id, username, location FROM users WHERE id = 5",
      "SELECT id, username FROM posts WHERE id = 6",
      "SELECT id, username FROM engagements WHERE id = 8",
      "UPDATE users SET location = username, username = location WHERE id = 5",
      // A chain: A becomes B while B becomes C; references follow one step.
      "UPDATE users SET username = username || 'x' WHERE id = 1 OR id = 6",
      "SELECT username, COUNT(*) FROM posts GROUP BY username ORDER BY 1",
      "SELECT username, COUNT(*) FROM engagements GROUP BY username ORDER BY "
      "1",
      "UPDATE users SET username = SUBSTR(username, 1, LENGTH(username) - 1) "
      "WHERE id = 1 OR id = 6",
      "SELECT username, COUNT(*) FROM posts GROUP BY username ORDER BY 1",
      "UPDATE users SET username = 'bob' WHERE id = 3",
      "UPDATE users SET username = username || 'x' WHERE id = 2",
      "UPDATE users SET username = SUBSTR(username, 1, 3) WHERE id = 2",
      "UPDATE posts SET username = 'ghost' WHERE id = 1",
      "UPDATE posts SET username = 'eve' WHERE id = 1",
      "UPDATE engagements SET postid = 999 WHERE id = 1",
      "UPDATE engagements SET postid = 2 WHERE id = 1",
      "UPDATE posts SET views = 100 / (views - views)",
      "UPDATE posts SET views = 0 WHERE 10 / (views - views) > 1",
      "UPDATE engagements SET timestamp = timestamp + 1",
      "UPDATE posts SET views = views * 9223372036854775807 WHERE views > 1",
      // Rows that grow from a few bytes to over a thousand, then shrink.
      "UPDATE posts SET content = content || content WHERE LENGTH(content) < "
      "1000",
      "UPDATE posts SET content = content || content WHERE LENGTH(content) < "
      "1000",
      "UPDATE posts SET content = content || content WHERE LENGTH(content) < "
      "1000",
      "UPDATE posts SET content = content || content WHERE LENGTH(content) < "
      "1000",
      "UPDATE posts SET content = content || content WHERE LENGTH(content) < "
      "1000",
      "UPDATE posts SET content = content || content WHERE LENGTH(content) < "
      "1000",
      "SELECT id, LENGTH(content) FROM posts ORDER BY id",
      "SELECT username, COUNT(*), MAX(LENGTH(content)) FROM posts GROUP BY "
      "username ORDER BY 1",
      "UPDATE posts SET content = SUBSTR(content, 1, 12)",
      "SELECT id, content FROM posts ORDER BY id",
  };
  return corpus;
}

// DELETE: RESTRICT, a delete that frees a slot for the next INSERT.
const std::vector<std::string>& DeleteCorpus() {
  static const std::vector<std::string> corpus = {
      "DELETE FROM engagements WHERE type = 'share'",
      "DELETE FROM users WHERE id = 4",
      "DELETE FROM posts WHERE id = 5",
      "DELETE FROM engagements WHERE postid = 5",
      "DELETE FROM posts WHERE id = 5",
      "DELETE FROM users WHERE id = 4",
      "DELETE FROM engagements WHERE username = 'dan'",
      "DELETE FROM users WHERE id = 4",
      "DELETE FROM engagements WHERE id = 9999",
      "DELETE FROM posts WHERE id = 8",
      "INSERT INTO posts VALUES (8, 'reused', 'bob', 1)",
      "SELECT * FROM posts ORDER BY id",
      "DELETE FROM nosuch",
      "DELETE FROM users WHERE username = 1",
      "SELECT COUNT(*) FROM users",
      "SELECT COUNT(*), SUM(views) FROM posts",
      "SELECT COUNT(*) FROM engagements",
  };
  return corpus;
}

// The sign of a DOUBLE zero. -0.0 and 0.0 compare equal, so an assignment equal
// to the stored value keeps the stored cell, also when another cell of the row
// changes. The comparison of cells cannot see the sign, so CAST prints it.
const std::vector<std::string>& ZeroCorpus() {
  static const std::vector<std::string> corpus = {
      "CREATE TABLE readings (id INT PRIMARY KEY, d DOUBLE, n INT)",
      "INSERT INTO readings VALUES (1, 0.0, 1), (2, -0.0, 1), (3, -0.0, 1), "
      "(4, -1.5, 1)",
      "UPDATE readings SET d = -0.0, n = 2 WHERE id = 1",
      "UPDATE readings SET d = 0, n = 6 WHERE id = 2",
      "UPDATE readings SET d = ABS(d)",
      "SELECT id, CAST(d AS TEXT), n FROM readings ORDER BY id",
  };
  return corpus;
}

// Runs the whole corpus on both engines over a pool of 4 frames and the given
// replacement policy, then compares every table once more through SELECT *.
void RunFixedCorpus(ReplacementPolicyKind policy) {
  EnginePair pair;
  if (!OpenPair(&pair, 4, policy)) {
    return;
  }
  EquivalenceRunner runner(*pair.flatfile, *pair.paged.service,
                           std::string("fixed corpus, ") + PolicyName(policy));
  Outcomes outcomes;
  const std::vector<const std::vector<std::string>*> sections = {
      &EquivalenceSetupStatements(),
      &ReadCorpus(),
      &OrderCorpus(),
      &JoinCorpus(),
      &AggregateCorpus(),
      &InsertCorpus(),
      &DefinitionCorpus(),
      &UpdateCorpus(),
      &ZeroCorpus(),
      &DeleteCorpus()};
  for (const std::vector<std::string>* section : sections) {
    for (const std::string& sql : *section) {
      if (!RUN_EQUIVALENT(runner, sql)) {
        return;
      }
      CountOutcome(sql, runner.last_flatfile_result(), &outcomes);
    }
  }
  for (const domain::TableSchema& schema : pair.flatfile->DescribeCatalog()) {
    if (!RUN_EQUIVALENT(runner, "SELECT * FROM " + schema.name)) {
      return;
    }
  }

  // The corpus reaches every kind of outcome of 2.6 and the constraint rules.
  CHECK(outcomes.selects_with_rows >= 50);
  CHECK(outcomes.selects_without_rows >= 3);
  CHECK(outcomes.ordered_limit_selects >= 6);
  CHECK(outcomes.writes_applied >= 40);
  CHECK(outcomes.writes_matching_no_row >= 3);
  CHECK(outcomes.bind_errors >= 8);
  CHECK(outcomes.runtime_errors >= 6);
  CHECK(outcomes.duplicate_keys >= 6);
  CHECK(outcomes.missing_parents >= 5);
  CHECK(outcomes.restricted_deletes >= 4);
  // The corpus avoids the statements of the difference table, which have
  // their own test.
  CHECK_EQ(outcomes.unsupported_errors, std::size_t{1});  // UPDATE of the key
  CHECK_EQ(outcomes.other_errors, std::size_t{0});
}

// ----------------------------------------------------------------------------
// The known differences, each as the design document records it.
// ----------------------------------------------------------------------------

// The two results of one statement.
struct BothResults {
  ExecutionResult flatfile;
  ExecutionResult paged;
};

BothResults RunBoth(EnginePair* pair, const std::string& sql) {
  return BothResults{ExecuteSql(*pair->flatfile, sql),
                     ExecuteSql(*pair->paged.service, sql)};
}

// Runs sql on both engines and reports a failure unless they are equivalent.
bool ExpectSame(EnginePair* pair, const std::string& sql) {
  const BothResults results = RunBoth(pair, sql);
  return ReportDifference(
      __FILE__, __LINE__,
      FindResultDifference(results.flatfile, results.paged, {}));
}

// Reports a failure unless result is the error of this kind and message.
void ExpectError(const ExecutionResult& result, ErrorKind kind,
                 const std::string& message) {
  const ExecutionError* error = AsError(result);
  if (error == nullptr) {
    ReportFailure(__FILE__, __LINE__,
                  "expected an error, got " + DescribeResult(result));
    return;
  }
  CHECK_EQ(std::string(domain::ErrorKindName(error->kind)),
           std::string(domain::ErrorKindName(kind)));
  CHECK_EQ(error->message, message);
}

void ExpectAffected(const ExecutionResult& result, std::size_t count) {
  const domain::AffectedRows* affected = AsAffectedRows(result);
  if (affected == nullptr) {
    ReportFailure(__FILE__, __LINE__,
                  "expected affected=" + std::to_string(count) + ", got " +
                      DescribeResult(result));
    return;
  }
  CHECK_EQ(affected->count, count);
}

void ExpectComplete(const ExecutionResult& result, const std::string& tag) {
  const domain::CommandComplete* complete = AsCommandComplete(result);
  if (complete == nullptr) {
    ReportFailure(
        __FILE__, __LINE__,
        "expected complete " + tag + ", got " + DescribeResult(result));
    return;
  }
  CHECK_EQ(complete->tag, tag);
}

// The text of the first column of every row, in the engine's own order.
std::vector<std::string> FirstColumn(const ExecutionResult& result) {
  std::vector<std::string> cells;
  const domain::ResultSet* rows = AsResultSet(result);
  if (rows == nullptr) {
    ReportFailure(__FILE__, __LINE__,
                  "expected a result set, got " + DescribeResult(result));
    return cells;
  }
  for (const Row& row : rows->rows) {
    cells.push_back(domain::FormatValue(row[0]));
  }
  return cells;
}

// The first element of `order` that is one of `wanted`.
std::string FirstOf(const std::vector<std::string>& order,
                    const std::vector<std::string>& wanted) {
  for (const std::string& cell : order) {
    for (const std::string& candidate : wanted) {
      if (cell == candidate) {
        return cell;
      }
    }
  }
  return "";
}

// TEXT with a line break (ADR-0012 against ADR-0027).
void CheckLineBreakDifference(EnginePair* pair) {
  CHECK(ExpectSame(pair, "CREATE TABLE notes (id INT PRIMARY KEY, body TEXT)"));
  const BothResults newline =
      RunBoth(pair, "INSERT INTO notes VALUES (1, 'line one\nline two')");
  ExpectError(newline.flatfile, ErrorKind::kUnsupported,
              "flatfile engine cannot store a line break in notes.body");
  ExpectAffected(newline.paged, 1);
  const BothResults carriage_return =
      RunBoth(pair, "INSERT INTO notes VALUES (2, 'carriage\rreturn')");
  ExpectError(carriage_return.flatfile, ErrorKind::kUnsupported,
              "flatfile engine cannot store a line break in notes.body");
  ExpectAffected(carriage_return.paged, 1);

  // flatfile stored neither row; paged stores both and returns them intact.
  const BothResults counts = RunBoth(pair, "SELECT id FROM notes");
  CHECK_EQ(FirstColumn(counts.flatfile).size(), std::size_t{0});
  CHECK_EQ(FirstColumn(counts.paged).size(), std::size_t{2});
  const BothResults body = RunBoth(pair, "SELECT body FROM notes WHERE id = 1");
  const domain::ResultSet* body_rows = AsResultSet(body.paged);
  CHECK(body_rows != nullptr && body_rows->rows.size() == 1 &&
        body_rows->rows[0][0] == Value(std::string("line one\nline two")));
}

// A tuple above 4060 bytes (ADR-0027).
void CheckRowLengthDifference(EnginePair* pair) {
  CHECK(ExpectSame(pair, "CREATE TABLE wide (id INT PRIMARY KEY, body TEXT)"));
  const std::string insert_long =
      "INSERT INTO wide VALUES (1, '" + std::string(4100, 'x') + "')";
  const BothResults too_long = RunBoth(pair, insert_long);
  ExpectAffected(too_long.flatfile, 1);
  ExpectError(too_long.paged, ErrorKind::kUnsupported,
              "paged engine cannot store a row longer than 4060 bytes in wide");
  // 4050 bytes of TEXT make a 4060-byte tuple, which both engines store.
  const BothResults at_limit = RunBoth(
      pair, "INSERT INTO wide VALUES (2, '" + std::string(4050, 'y') + "')");
  ExpectAffected(at_limit.flatfile, 1);
  ExpectAffected(at_limit.paged, 1);
  const BothResults lengths = RunBoth(pair, "SELECT LENGTH(body) FROM wide");
  CHECK_EQ(FirstColumn(lengths.flatfile),
           std::vector<std::string>({"4100", "4050"}));
  CHECK_EQ(FirstColumn(lengths.paged), std::vector<std::string>({"4050"}));
}

// The reserved prefix, and the two statements that the paged engine does not
// support yet (ALTER TABLE ADD COLUMN and DROP TABLE).
void CheckDdlDifferences(EnginePair* pair) {
  const BothResults reserved =
      RunBoth(pair, "CREATE TABLE sys_x (id INT PRIMARY KEY)");
  ExpectComplete(reserved.flatfile, "CREATE TABLE sys_x");
  const ExecutionError* reserved_error = AsError(reserved.paged);
  CHECK(reserved_error != nullptr &&
        reserved_error->kind == ErrorKind::kUnsupported &&
        Contains(reserved_error->message, "sys_"));

  CHECK(ExpectSame(pair, "CREATE TABLE spare (id INT PRIMARY KEY, v INT)"));
  CHECK(ExpectSame(pair, "INSERT INTO spare VALUES (1, 10)"));
  const BothResults alter =
      RunBoth(pair, "ALTER TABLE spare ADD COLUMN extra INT DEFAULT 0");
  ExpectComplete(alter.flatfile, "ALTER TABLE spare");
  ExpectError(alter.paged, ErrorKind::kUnsupported,
              "paged engine does not support ALTER TABLE ADD COLUMN");
  const BothResults drop = RunBoth(pair, "DROP TABLE spare");
  ExpectComplete(drop.flatfile, "DROP TABLE spare");
  ExpectError(drop.paged, ErrorKind::kUnsupported,
              "paged engine does not support DROP TABLE");

  // The refused statements left the paged table as it was; flatfile dropped it.
  const BothResults after = RunBoth(pair, "SELECT * FROM spare");
  CHECK(AsError(after.flatfile) != nullptr &&
        AsError(after.flatfile)->kind == ErrorKind::kBind);
  const domain::ResultSet* kept = AsResultSet(after.paged);
  CHECK(kept != nullptr && kept->columns.size() == 2 && kept->rows.size() == 1);
}

// Rows below a LIMIT that are never read (ADR-0036).
void CheckEarlyTerminationDifference(EnginePair* pair) {
  CHECK(ExpectSame(pair, "CREATE TABLE ratio (id INT PRIMARY KEY, x INT)"));
  CHECK(ExpectSame(pair, "INSERT INTO ratio VALUES (1, 5), (2, 0), (3, 2)"));
  const BothResults limited = RunBoth(pair, "SELECT 1 / x FROM ratio LIMIT 1");
  ExpectError(limited.flatfile, ErrorKind::kRuntime, "division by zero");
  const domain::ResultSet* one_row = AsResultSet(limited.paged);
  CHECK(one_row != nullptr && one_row->rows.size() == 1);
  // Under a Sort every row is read, so both report the error.
  CHECK(ExpectSame(pair, "SELECT 1 / x FROM ratio ORDER BY 1 LIMIT 1"));
  // LIMIT 0 reads no row at all, not even below a Sort: only flatfile reports.
  const BothResults none =
      RunBoth(pair, "SELECT 1 / x FROM ratio ORDER BY 1 LIMIT 0");
  ExpectError(none.flatfile, ErrorKind::kRuntime, "division by zero");
  const domain::ResultSet* no_rows = AsResultSet(none.paged);
  CHECK(no_rows != nullptr && no_rows->rows.empty());
}

// When several rows violate a constraint, each engine reports the first one in
// its own order; the kind is the same. A delete and an insert make the order
// of paged (slot order) differ from the order of flatfile (file order).
void CheckViolationOrderDifference(EnginePair* pair) {
  CHECK(ExpectSame(pair,
                   "CREATE TABLE parents (id INT PRIMARY KEY, name TEXT "
                   "UNIQUE)"));
  CHECK(ExpectSame(pair,
                   "CREATE TABLE children (id INT PRIMARY KEY, parent "
                   "TEXT REFERENCES parents(name))"));
  CHECK(ExpectSame(pair,
                   "INSERT INTO parents VALUES (1, 'filler'), (2, 'a'), "
                   "(3, 'ax'), (4, 'bx')"));
  CHECK(ExpectSame(pair, "DELETE FROM parents WHERE id = 1"));
  CHECK(ExpectSame(pair, "INSERT INTO parents VALUES (5, 'b')"));
  CHECK(ExpectSame(pair, "INSERT INTO children VALUES (1, 'a'), (2, 'b')"));

  const BothResults order = RunBoth(pair, "SELECT id FROM parents");
  const std::vector<std::string> flatfile_order = FirstColumn(order.flatfile);
  const std::vector<std::string> paged_order = FirstColumn(order.paged);
  CHECK_EQ(flatfile_order, std::vector<std::string>({"2", "3", "4", "5"}));
  // The scenario only means something when the two orders differ.
  CHECK(flatfile_order != paged_order);

  // DELETE with two referenced rows (a: id 2, b: id 5).
  const BothResults restricted = RunBoth(pair, "DELETE FROM parents");
  const ExecutionError* flatfile_error = AsError(restricted.flatfile);
  const ExecutionError* paged_error = AsError(restricted.paged);
  CHECK(flatfile_error != nullptr && paged_error != nullptr);
  if (flatfile_error != nullptr && paged_error != nullptr) {
    CHECK(flatfile_error->kind == ErrorKind::kConstraintViolation);
    CHECK(paged_error->kind == ErrorKind::kConstraintViolation);
    CHECK_EQ(flatfile_error->message,
             std::string("parents.id=2 is referenced by children.parent"));
    // Each engine names the first referenced row of its own order.
    CHECK_EQ(paged_error->message,
             "parents.id=" + FirstOf(paged_order, {"2", "5"}) +
                 " is referenced by children.parent");
    CHECK(flatfile_error->message != paged_error->message);
  }

  // UPDATE where 'a' and 'b' both become a name that exists: 'ax' and 'bx'.
  const BothResults renamed = RunBoth(
      pair,
      "UPDATE parents SET name = name || 'x' WHERE name = 'a' OR name = 'b'");
  const ExecutionError* flatfile_rename = AsError(renamed.flatfile);
  const ExecutionError* paged_rename = AsError(renamed.paged);
  CHECK(flatfile_rename != nullptr && paged_rename != nullptr);
  if (flatfile_rename != nullptr && paged_rename != nullptr) {
    CHECK(flatfile_rename->kind == ErrorKind::kConstraintViolation);
    CHECK(paged_rename->kind == ErrorKind::kConstraintViolation);
    // Row 2 (a) becomes ax and row 5 (b) becomes bx; both names exist.
    CHECK_EQ(flatfile_rename->message,
             FirstOf(flatfile_order, {"2", "5"}) == "2"
                 ? std::string("duplicate key: parents.name=ax")
                 : std::string("duplicate key: parents.name=bx"));
    CHECK_EQ(paged_rename->message,
             FirstOf(paged_order, {"2", "5"}) == "2"
                 ? std::string("duplicate key: parents.name=ax")
                 : std::string("duplicate key: parents.name=bx"));
    CHECK(flatfile_rename->message != paged_rename->message);
  }
}

// CREATE TABLE next to a CSV file of the same name: flatfile adopts its rows,
// paged has no concept of CSV and creates an empty table.
void CheckAdoptedCsvDifference() {
  TempDir directory;
  WriteFile(directory.path() / "t.csv", "id,name\n1,a\n2,b\n");
  std::unique_ptr<QueryService> flatfile = MakeLoadedService(directory.path());
  PagedFixture paged =
      MakeFilePagedFixture(directory.path(), 16, ReplacementPolicyKind::kLru);
  if (flatfile == nullptr || paged.service == nullptr) {
    return;
  }
  const std::string create = "CREATE TABLE t (id INT PRIMARY KEY, name TEXT)";
  ExpectComplete(ExecuteSql(*flatfile, create),
                 "CREATE TABLE t (adopted 2 rows)");
  ExpectComplete(ExecuteSql(*paged.service, create), "CREATE TABLE t");
  CHECK_EQ(FirstColumn(ExecuteSql(*flatfile, "SELECT COUNT(*) FROM t")),
           std::vector<std::string>{"2"});
  CHECK_EQ(FirstColumn(ExecuteSql(*paged.service, "SELECT COUNT(*) FROM t")),
           std::vector<std::string>{"0"});
}

std::vector<std::string> RunRepl(const std::vector<std::string>& input_lines) {
  std::string text;
  for (const std::string& line : input_lines) {
    text += line + "\n";
  }
  std::istringstream input(text);
  std::ostringstream output;
  cli::RunRepl(input, output, false);
  std::vector<std::string> lines;
  std::istringstream reader(output.str());
  for (std::string line; std::getline(reader, line);) {
    lines.push_back(line);
  }
  return lines;
}

bool AnyLineStartsWith(const std::vector<std::string>& lines,
                       const std::string& prefix) {
  for (const std::string& line : lines) {
    if (line.starts_with(prefix)) {
      return true;
    }
  }
  return false;
}

// `.open <directory> --parallel`: flatfile loads its tables in parallel and
// paged, which has no load phase, accepts the option and ignores it.
void CheckParallelOption() {
  TempDir directory;
  WriteFixtureA(directory.path());
  const std::string open = ".open " + directory.path().string() + " --parallel";

  const std::vector<std::string> flatfile = RunRepl({open});
  CHECK_EQ(flatfile,
           std::vector<std::string>{"loaded users=4 posts=3 engagements=5"});

  const std::vector<std::string> paged = RunRepl({".engine paged", open});
  CHECK(paged.size() >= 2 && paged[0] == "engine paged" &&
        paged[1] == "loaded no tables");
  CHECK(!AnyLineStartsWith(paged, "error"));
}

// What reaches the file when a write statement returns (design document 2.8):
// both engines write it out at once, paged without a sync. The storage layer
// states the file is complete after Close, so the file is read again after
// Close too.
void CheckDurabilityDifference() {
  TempDir flatfile_directory;
  TempDir paged_directory;
  TempDir copy_directory;
  std::unique_ptr<QueryService> flatfile =
      MakeLoadedService(flatfile_directory.path());
  PagedFixture paged = MakeFilePagedFixture(paged_directory.path(), 16,
                                            ReplacementPolicyKind::kLru);
  if (flatfile == nullptr || paged.service == nullptr) {
    return;
  }
  const std::filesystem::path paged_file = paged_directory.path() / "paged.db";
  const std::string bytes_at_open = ReadFile(paged_file);

  const std::string create = "CREATE TABLE d (id INT PRIMARY KEY, v TEXT)";
  const std::string insert = "INSERT INTO d VALUES (1, 'persisted')";
  CHECK(AsCommandComplete(ExecuteSql(*flatfile, create)) != nullptr);
  CHECK(AsCommandComplete(ExecuteSql(*paged.service, create)) != nullptr);
  ExpectAffected(ExecuteSql(*flatfile, insert), 1);
  ExpectAffected(ExecuteSql(*paged.service, insert), 1);

  // Before Close: the CSV file holds the row, and so does paged.db.
  CHECK(Contains(ReadFile(flatfile_directory.path() / "d.csv"), "1,persisted"));
  CHECK(ReadFile(paged_file) != bytes_at_open);
  std::error_code error;
  std::filesystem::copy_file(paged_file, copy_directory.path() / "paged.db",
                             error);
  CHECK(!error);
  {
    PagedFixture copy = MakeFilePagedFixture(copy_directory.path(), 16,
                                             ReplacementPolicyKind::kLru);
    if (copy.service == nullptr) {
      return;
    }
    CHECK_EQ(FirstColumn(ExecuteSql(*copy.service, "SELECT v FROM d")),
             std::vector<std::string>{"persisted"});
  }

  // After Close (the destruction of the engine) the file still holds the row.
  paged.service.reset();
  paged = MakeFilePagedFixture(paged_directory.path(), 16,
                               ReplacementPolicyKind::kLru);
  if (paged.service == nullptr) {
    return;
  }
  CHECK_EQ(FirstColumn(ExecuteSql(*paged.service, "SELECT v FROM d")),
           std::vector<std::string>{"persisted"});
}

// ----------------------------------------------------------------------------
// Random statements.
// ----------------------------------------------------------------------------

constexpr std::size_t kStatementsPerSeed = 1500;

struct RandomRun {
  std::uint64_t seed;
  ReplacementPolicyKind policy;
  std::size_t frames;
};

// The setup, then kStatementsPerSeed generated statements, compared one by one.
// The sequence number in a difference counts the setup statements too.
void RunRandomStatements(const RandomRun& run) {
  EnginePair pair;
  if (!OpenPair(&pair, run.frames, run.policy)) {
    return;
  }
  EquivalenceRunner runner(*pair.flatfile, *pair.paged.service,
                           "seed " + std::to_string(run.seed) + ", " +
                               PolicyName(run.policy) + ", " +
                               std::to_string(run.frames) + " frames");
  for (const std::string& sql : EquivalenceSetupStatements()) {
    if (!RUN_EQUIVALENT(runner, sql)) {
      return;
    }
  }
  StatementGenerator generator(run.seed);
  Outcomes outcomes;
  for (std::size_t i = 0; i < kStatementsPerSeed; ++i) {
    const std::string sql = generator.Next(runner.flatfile_tables());
    if (!RUN_EQUIVALENT(runner, sql)) {
      return;
    }
    CountOutcome(sql, runner.last_flatfile_result(), &outcomes);
  }
  for (const domain::TableSchema& schema : pair.flatfile->DescribeCatalog()) {
    if (!RUN_EQUIVALENT(runner, "SELECT * FROM " + schema.name)) {
      return;
    }
  }

  // A stream that stopped reaching the rules (every statement failing to bind,
  // say) would pass vacuously, so the outcomes are checked.
  CHECK(outcomes.selects_with_rows >= 200);
  CHECK(outcomes.selects_without_rows >= 20);
  CHECK(outcomes.ordered_limit_selects >= 30);
  CHECK(outcomes.writes_applied >= 250);
  CHECK(outcomes.writes_matching_no_row >= 10);
  CHECK(outcomes.runtime_errors >= 15);
  CHECK(outcomes.duplicate_keys >= 15);
  CHECK(outcomes.missing_parents >= 8);
  CHECK(outcomes.restricted_deletes >= 5);
  CHECK_EQ(outcomes.other_errors, std::size_t{0});
}

}  // namespace

TEST(test_equivalence_fixed_corpus_matches_on_both_engines) {
  CheckComparatorFollowsTheRules();
  CheckRunnerComparesTables();
  RunFixedCorpus(ReplacementPolicyKind::kLru);
  RunFixedCorpus(ReplacementPolicyKind::kTwoQ);
}

TEST(test_equivalence_known_differences_are_as_documented) {
  EnginePair pair;
  if (!OpenPair(&pair, 4, ReplacementPolicyKind::kLru)) {
    return;
  }
  CheckLineBreakDifference(&pair);
  CheckRowLengthDifference(&pair);
  CheckDdlDifferences(&pair);
  CheckEarlyTerminationDifference(&pair);
  CheckViolationOrderDifference(&pair);
  CheckAdoptedCsvDifference();
  CheckParallelOption();
  CheckDurabilityDifference();
}

TEST(test_equivalence_randomized_statements_match) {
  RunRandomStatements({20260101, ReplacementPolicyKind::kLru, 4});
  RunRandomStatements({20260202, ReplacementPolicyKind::kTwoQ, 4});
  RunRandomStatements({20260303, ReplacementPolicyKind::kLru, 16});
}

}  // namespace flatfiledb::testing
