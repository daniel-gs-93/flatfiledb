#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "strata/application.h"
#include "strata/domain/binder.h"
#include "strata/flat_file_engine.h"
#include "test_harness.h"
#include "test_support.h"

// Integration tests of FlatFileEngine driven through the parser, the Binder
// and CsvRepository over temporary CSV files.

namespace strata::testing {

namespace {

using application::QueryService;
using domain::ErrorKind;
using domain::ExecutionError;
using domain::ExecutionResult;

constexpr const char* kAllUsers =
    "SELECT id, username, location FROM users ORDER BY id";
constexpr const char* kAllPosts =
    "SELECT id, content, username, views FROM posts ORDER BY id";
constexpr const char* kAllEngagements =
    "SELECT id, postId, username, type, comment, timestamp FROM engagements "
    "ORDER BY id";
constexpr const char* kInsertEngagement =
    "INSERT INTO engagements (id, postId, username, type, comment, timestamp) "
    "VALUES ";

const char* KindName(ErrorKind kind) { return domain::ErrorKindName(kind); }

std::string Describe(const ExecutionResult& result) {
  if (const ExecutionError* error = AsError(result)) {
    return std::string("error ") + KindName(error->kind) + ": " +
           error->message;
  }
  if (const domain::AffectedRows* affected = AsAffectedRows(result)) {
    return "affected=" + std::to_string(affected->count);
  }
  if (const domain::CommandComplete* complete = AsCommandComplete(result)) {
    return "complete " + complete->tag;
  }
  return "a result set";
}

// Returns the error when result is one of the given kind, otherwise reports a
// failure and returns null.
const ExecutionError* ExpectError(const ExecutionResult& result,
                                  ErrorKind kind) {
  const ExecutionError* error = AsError(result);
  if (error == nullptr) {
    ReportFailure(__FILE__, __LINE__,
                  std::string("expected error ") + KindName(kind) +
                      " but got " + Describe(result));
    return nullptr;
  }
  CHECK_EQ(std::string(KindName(error->kind)), std::string(KindName(kind)));
  return error;
}

void ExpectErrorMessage(const ExecutionResult& result, ErrorKind kind,
                        const std::string& message) {
  const ExecutionError* error = ExpectError(result, kind);
  if (error == nullptr) {
    return;
  }
  CHECK_EQ(error->message, message);
}

void ExpectComplete(const ExecutionResult& result, const std::string& tag) {
  const domain::CommandComplete* complete = AsCommandComplete(result);
  if (complete == nullptr) {
    ReportFailure(__FILE__, __LINE__,
                  "expected complete " + tag + " but got " + Describe(result));
    return;
  }
  CHECK_EQ(complete->tag, tag);
}

void ExpectAffected(const ExecutionResult& result, std::size_t count) {
  const domain::AffectedRows* affected = AsAffectedRows(result);
  if (affected == nullptr) {
    ReportFailure(__FILE__, __LINE__,
                  "expected affected=" + std::to_string(count) + " but got " +
                      Describe(result));
    return;
  }
  CHECK_EQ(affected->count, count);
}

// The rows of a SELECT as Tab-joined lines; empty, with a reported failure,
// when the statement does not produce a result set.
std::vector<std::string> QueryLines(QueryService& service,
                                    const std::string& sql) {
  ExecutionResult result = ExecuteSql(service, sql);
  const domain::ResultSet* result_set = AsResultSet(result);
  if (result_set == nullptr) {
    ReportFailure(
        __FILE__, __LINE__,
        "expected a result set for [" + sql + "] but got " + Describe(result));
    return {};
  }
  return RowsAsText(*result_set);
}

std::vector<std::string> ColumnNames(const ExecutionResult& result) {
  std::vector<std::string> names;
  const domain::ResultSet* result_set = AsResultSet(result);
  if (result_set == nullptr) {
    return names;
  }
  for (const domain::ColumnDef& column : result_set->columns) {
    names.push_back(column.name);
  }
  return names;
}

std::string Join(const std::vector<std::string>& lines) {
  std::string joined;
  for (const std::string& line : lines) {
    joined += line + "\n";
  }
  return joined;
}

// Every row of the three tables in id order, joined into one string.
std::string SnapshotTables(QueryService& service) {
  std::string snapshot;
  for (const char* sql : {kAllUsers, kAllPosts, kAllEngagements}) {
    snapshot += Join(QueryLines(service, sql)) + "--\n";
  }
  return snapshot;
}

std::string ReadAllCsv(const std::filesystem::path& dir) {
  return ReadFile(dir / "users.csv") + "=\n" + ReadFile(dir / "posts.csv") +
         "=\n" + ReadFile(dir / "engagements.csv");
}

std::string CountRows(QueryService& service, const std::string& table) {
  std::vector<std::string> lines =
      QueryLines(service, "SELECT COUNT(*) FROM " + table);
  return lines.empty() ? std::string("<none>") : lines[0];
}

// "<id><Tab><type>" lines for the ids first_id, first_id + 2, ... below
// kEndId, the rows of one key group in table order.
constexpr int kEndId = 260;

std::string IdsWithType(int first_id, const std::string& type) {
  std::string lines;
  for (int id = first_id; id < kEndId; id += 2) {
    lines += std::to_string(id) + "\t" + type + "\n";
  }
  return lines;
}

void ExpectForeignKey(const domain::ForeignKey& foreign_key,
                      std::size_t column_index, const std::string& ref_table,
                      std::size_t ref_column_index) {
  CHECK_EQ(foreign_key.column_index, column_index);
  CHECK_EQ(foreign_key.ref_table, ref_table);
  CHECK_EQ(foreign_key.ref_column_index, ref_column_index);
}

domain::PlanNode InsertPlan(const std::string& table, domain::Row row) {
  domain::PlanNode plan;
  plan.node = domain::InsertNode{table, {std::move(row)}};
  return plan;
}

domain::Row PostRow(int64_t id, const std::string& content,
                    const std::string& username, int64_t views) {
  return domain::Row{domain::Value(id), domain::Value(content),
                     domain::Value(username), domain::Value(views)};
}

void ExpectLoadCounts(const infrastructure::LoadResult& loaded) {
  CHECK(loaded.is_success);
  CHECK_EQ(loaded.tables.size(), std::size_t{3});
  if (loaded.tables.size() != 3) {
    return;
  }
  CHECK_EQ(loaded.tables[0].name, std::string("users"));
  CHECK_EQ(loaded.tables[0].row_count, std::size_t{4});
  CHECK_EQ(loaded.tables[1].name, std::string("posts"));
  CHECK_EQ(loaded.tables[1].row_count, std::size_t{3});
  CHECK_EQ(loaded.tables[2].name, std::string("engagements"));
  CHECK_EQ(loaded.tables[2].row_count, std::size_t{5});
}

}  // namespace

TEST(test_engine_catalog_registers_three_tables_with_keys) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  infrastructure::FlatFileEngine engine(MakeCsvRepository(dir.path()));

  // Act
  infrastructure::LoadResult loaded = engine.Load(false);
  const domain::ICatalog& catalog = engine.Catalog();
  std::vector<std::string> names = catalog.TableNames();
  std::optional<domain::TableSchema> users = catalog.FindTable("users");
  std::optional<domain::TableSchema> posts = catalog.FindTable("posts");
  std::optional<domain::TableSchema> engagements =
      catalog.FindTable("engagements");

  // Assert
  CHECK(loaded.is_success);
  CHECK_EQ(Join(names), std::string("users\nposts\nengagements\n"));
  CHECK(users.has_value() && posts.has_value() && engagements.has_value());
  if (!users || !posts || !engagements) {
    return;
  }
  CHECK_EQ(users->primary_key_index, std::size_t{0});
  CHECK_EQ(posts->primary_key_index, std::size_t{0});
  CHECK_EQ(engagements->primary_key_index, std::size_t{0});
  CHECK_EQ(users->foreign_keys.size(), std::size_t{0});
  CHECK_EQ(users->unique_indexes, std::vector<std::size_t>({1}));
  CHECK(!users->auto_increment_index.has_value());
  CHECK(!posts->auto_increment_index.has_value());
  CHECK_EQ(posts->foreign_keys.size(), std::size_t{1});
  CHECK_EQ(engagements->foreign_keys.size(), std::size_t{2});
  if (posts->foreign_keys.size() != 1 ||
      engagements->foreign_keys.size() != 2) {
    return;
  }
  ExpectForeignKey(posts->foreign_keys[0], 2, "users", 1);
  ExpectForeignKey(engagements->foreign_keys[0], 1, "posts", 0);
  ExpectForeignKey(engagements->foreign_keys[1], 2, "users", 1);
  CHECK(catalog.FindTable("USERS").has_value());
}

TEST(test_engine_select_with_where_returns_row) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result =
      ExecuteSql(*service, "SELECT username, location FROM users WHERE id = 1");

  // Assert
  const domain::ResultSet* result_set = AsResultSet(result);
  CHECK(result_set != nullptr);
  if (result_set == nullptr) {
    return;
  }
  CHECK_EQ(Join(ColumnNames(result)), std::string("username\nlocation\n"));
  CHECK_EQ(Join(RowsAsText(*result_set)), std::string("alice\tParis\n"));
}

TEST(test_engine_two_table_join_with_order_by) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result =
      ExecuteSql(*service,
                 "SELECT e.postId, e.comment FROM engagements e JOIN users u "
                 "ON e.username = u.username "
                 "WHERE u.location = 'Paris' ORDER BY e.postId, e.comment");

  // Assert
  const domain::ResultSet* result_set = AsResultSet(result);
  CHECK(result_set != nullptr);
  if (result_set == nullptr) {
    return;
  }
  CHECK_EQ(Join(ColumnNames(result)), std::string("postId\ncomment\n"));
  CHECK_EQ(Join(RowsAsText(*result_set)),
           std::string("10\tnice\n12\tNone\n12\tNone\n"));
}

TEST(test_engine_group_by_count_over_join) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result =
      ExecuteSql(*service,
                 "SELECT e.type, COUNT(*) FROM engagements e JOIN users u ON "
                 "e.username = u.username "
                 "WHERE u.location = 'Paris' GROUP BY e.type ORDER BY e.type");

  // Assert
  const domain::ResultSet* result_set = AsResultSet(result);
  CHECK(result_set != nullptr);
  if (result_set == nullptr) {
    return;
  }
  CHECK_EQ(Join(ColumnNames(result)), std::string("type\nCOUNT(*)\n"));
  CHECK_EQ(Join(RowsAsText(*result_set)), std::string("comment\t1\nlike\t2\n"));
}

TEST(test_engine_three_table_join_returns_joined_rows) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  std::vector<std::string> rows = QueryLines(
      *service,
      "SELECT u.username, p.id, e.type FROM users u JOIN posts p ON "
      "p.username = u.username JOIN engagements e ON e.postId = p.id WHERE "
      "u.id = 1 ORDER BY p.id, e.type DESC");

  // Assert
  CHECK_EQ(
      Join(rows),
      std::string("alice\t10\tlike\nalice\t10\tcomment\nalice\t11\tcomment\n"));
}

TEST(test_engine_join_condition_kinds_return_correct_rows) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  std::vector<std::string> inequality_rows = QueryLines(
      *service, "SELECT u.id FROM users u JOIN posts p ON u.id < p.views");
  std::vector<std::string> equality_rows = QueryLines(
      *service,
      "SELECT u.id FROM users u JOIN posts p ON u.username = p.username");

  // Assert
  CHECK_EQ(inequality_rows.size(), std::size_t{8});
  CHECK_EQ(equality_rows.size(), std::size_t{3});
}

TEST(
    test_engine_join_on_equality_with_residual_condition_returns_correct_rows) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  std::vector<std::string> key_first =
      QueryLines(*service,
                 "SELECT p.id, e.id FROM posts p JOIN engagements e "
                 "ON e.postId = p.id AND e.type = 'like' ORDER BY p.id, e.id");
  std::vector<std::string> key_second =
      QueryLines(*service,
                 "SELECT p.id, e.id FROM posts p JOIN engagements e "
                 "ON e.type = 'like' AND e.postId = p.id ORDER BY p.id, e.id");
  std::vector<std::string> cross_table =
      QueryLines(*service,
                 "SELECT p.id, u.username FROM posts p JOIN users u "
                 "ON p.username = u.username AND u.id < p.views ORDER BY p.id");

  // Assert: left rows in order, matches in right-row order, residual applied.
  CHECK_EQ(Join(key_first), std::string("10\t100\n12\t103\n12\t104\n"));
  CHECK_EQ(Join(key_second), std::string("10\t100\n12\t103\n12\t104\n"));
  CHECK_EQ(Join(cross_table), std::string("10\talice\n12\tbob\n"));
}

TEST(test_engine_join_with_cross_table_or_filter_returns_correct_rows) {
  // Arrange: the OR spans both tables, so it stays above the join.
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  std::vector<std::string> equality_join = QueryLines(
      *service,
      "SELECT u.id, p.id FROM users u JOIN posts p "
      "ON u.username = p.username "
      "WHERE u.location = 'Rome' OR p.views = 0 ORDER BY u.id, p.id");
  std::vector<std::string> inequality_join = QueryLines(
      *service,
      "SELECT u.id, p.id FROM users u JOIN posts p ON u.id < p.views "
      "WHERE u.location = 'Rome' OR p.id = 12 ORDER BY u.id, p.id");

  // Assert
  CHECK_EQ(Join(equality_join), std::string("1\t11\n3\t12\n"));
  CHECK_EQ(Join(inequality_join),
           std::string("1\t12\n2\t12\n3\t10\n3\t12\n4\t12\n"));
}

TEST(test_engine_order_by_is_stable_and_respects_direction) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  std::vector<std::string> rows = QueryLines(
      *service, "SELECT id, type FROM engagements ORDER BY type DESC");

  // Assert
  CHECK_EQ(
      Join(rows),
      std::string(
          "100\tlike\n103\tlike\n104\tlike\n101\tcomment\n102\tcomment\n"));
}

TEST(test_engine_order_by_is_stable_for_many_rows) {
  // Arrange: 60 engagements in id order whose type alternates between two
  // values, so each sort key is shared by 30 rows.
  TempDir dir;
  WriteFixtureA(dir.path());
  std::string engagements = "id,postid,username,type,comment,timestamp\n";
  for (int id = 200; id < kEndId; ++id) {
    engagements += std::to_string(id) + ",10,alice," +
                   (id % 2 == 0 ? "even" : "odd") + ",None,1\n";
  }
  WriteFile(dir.path() / "engagements.csv", engagements);
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  std::vector<std::string> ascending =
      QueryLines(*service, "SELECT id, type FROM engagements ORDER BY type");
  std::vector<std::string> descending = QueryLines(
      *service, "SELECT id, type FROM engagements ORDER BY type DESC");

  // Assert: ids stay in table order inside each key group.
  CHECK_EQ(Join(ascending), IdsWithType(200, "even") + IdsWithType(201, "odd"));
  CHECK_EQ(Join(descending),
           IdsWithType(201, "odd") + IdsWithType(200, "even"));
}

TEST(test_engine_aggregate_over_empty_input) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  std::vector<std::string> ungrouped = QueryLines(
      *service, "SELECT COUNT(*), SUM(views) FROM posts WHERE id = 0");
  std::vector<std::string> grouped = QueryLines(
      *service,
      "SELECT type, COUNT(*) FROM engagements WHERE id = 0 GROUP BY type");

  // Assert
  CHECK_EQ(Join(ungrouped), std::string("0\t0\n"));
  CHECK_EQ(grouped.size(), std::size_t{0});
}

TEST(test_engine_integer_overflow_returns_runtime_error) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result = ExecuteSql(
      *service, "SELECT id + 9223372036854775807 FROM users WHERE id = 1");

  // Assert
  ExpectErrorMessage(result, ErrorKind::kRuntime, "numeric overflow");
}

TEST(test_engine_identifiers_case_insensitive_end_to_end) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result =
      ExecuteSql(*service, "SELECT postid FROM engagements WHERE POSTID = 10");

  // Assert
  const domain::ResultSet* result_set = AsResultSet(result);
  CHECK(result_set != nullptr);
  if (result_set == nullptr) {
    return;
  }
  CHECK_EQ(Join(ColumnNames(result)), std::string("postId\n"));
  CHECK_EQ(Join(RowsAsText(*result_set)), std::string("10\n10\n"));
}

TEST(test_engine_insert_valid_row_is_persisted) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result =
      ExecuteSql(*service, std::string(kInsertEngagement) +
                               "(105, 12, 'dave', '', 'None', 1005)");
  auto reloaded = MakeLoadedService(dir.path());
  if (!reloaded) {
    return;
  }

  // Assert
  ExpectAffected(result, 1);
  CHECK_EQ(CountRows(*reloaded, "engagements"), std::string("6"));
  CHECK_EQ(
      Join(QueryLines(*reloaded,
                      "SELECT id, postId, username, type, comment, timestamp "
                      "FROM engagements WHERE id = 105")),
      std::string("105\t12\tdave\t\tNone\t1005\n"));
  CHECK_EQ(
      Join(QueryLines(*reloaded, "SELECT id FROM engagements WHERE type = ''")),
      std::string("105\n"));
}

TEST(test_engine_insert_unknown_post_violates_constraint) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }
  std::string files_before = ReadAllCsv(dir.path());
  std::string memory_before = SnapshotTables(*service);

  // Act
  ExecutionResult result =
      ExecuteSql(*service, std::string(kInsertEngagement) +
                               "(105, 999, 'nobody', 'like', 'x', 1005)");

  // Assert
  ExpectErrorMessage(
      result, ErrorKind::kConstraintViolation,
      "no matching row: engagements.postId=999 not found in posts.id");
  CHECK_EQ(ReadAllCsv(dir.path()), files_before);
  CHECK_EQ(SnapshotTables(*service), memory_before);
}

TEST(test_engine_insert_duplicate_primary_key_violates_constraint) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }
  std::string files_before = ReadAllCsv(dir.path());

  // Act
  ExecutionResult result =
      ExecuteSql(*service,
                 "INSERT INTO posts (id, content, username, views) VALUES (10, "
                 "'dup', 'alice', 1)");

  // Assert
  ExpectErrorMessage(result, ErrorKind::kConstraintViolation,
                     "duplicate key: posts.id=10");
  CHECK_EQ(ReadAllCsv(dir.path()), files_before);
}

TEST(test_engine_insert_duplicate_referenced_value_violates_constraint) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }
  std::string files_before = ReadAllCsv(dir.path());

  // Act
  ExecutionResult result = ExecuteSql(
      *service,
      "INSERT INTO users (id, username, location) VALUES (9, 'bob', 'Rome')");

  // Assert
  ExpectErrorMessage(result, ErrorKind::kConstraintViolation,
                     "duplicate key: users.username=bob");
  CHECK_EQ(ReadAllCsv(dir.path()), files_before);
}

TEST(test_engine_insert_text_with_line_break_returns_unsupported) {
  // Arrange: the plans are built by hand so they do not depend on how the
  // parser treats a raw line break inside a literal.
  TempDir dir;
  WriteFixtureA(dir.path());
  infrastructure::FlatFileEngine engine(MakeCsvRepository(dir.path()));
  if (!engine.Load(false).is_success) {
    ReportFailure(__FILE__, __LINE__, "engine failed to load Fixture A");
    return;
  }
  std::string files_before = ReadAllCsv(dir.path());
  domain::PlanNode carriage_return_plan =
      InsertPlan("posts", PostRow(14, "a\rb", "alice", 1));
  domain::PlanNode line_feed_plan =
      InsertPlan("posts", PostRow(15, "a\nb", "alice", 1));

  // Act
  ExecutionResult carriage_return_result = engine.Execute(carriage_return_plan);
  ExecutionResult line_feed_result = engine.Execute(line_feed_plan);

  // Assert
  ExpectErrorMessage(
      carriage_return_result, ErrorKind::kUnsupported,
      "flatfile engine cannot store a line break in posts.content");
  ExpectErrorMessage(
      line_feed_result, ErrorKind::kUnsupported,
      "flatfile engine cannot store a line break in posts.content");
  CHECK_EQ(ReadAllCsv(dir.path()), files_before);
}

TEST(test_engine_int_values_beyond_32_bits_are_persisted) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult above_32_bits =
      ExecuteSql(*service,
                 "INSERT INTO posts (id, content, username, views) VALUES (20, "
                 "'p', 'alice', 2147483648)");
  ExecutionResult largest =
      ExecuteSql(*service,
                 "INSERT INTO posts (id, content, username, views) VALUES (21, "
                 "'q', 'alice', 9223372036854775807)");
  auto reloaded = MakeLoadedService(dir.path());
  if (!reloaded) {
    return;
  }

  // Assert
  ExpectAffected(above_32_bits, 1);
  ExpectAffected(largest, 1);
  CHECK_EQ(
      Join(QueryLines(
          *reloaded, "SELECT id, views FROM posts WHERE id >= 20 ORDER BY id")),
      std::string("20\t2147483648\n21\t9223372036854775807\n"));
}

TEST(test_engine_insert_checks_run_in_documented_order) {
  // Arrange: the line-break plan is built by hand (see the line-break test).
  TempDir dir;
  WriteFixtureA(dir.path());
  infrastructure::FlatFileEngine engine(MakeCsvRepository(dir.path()));
  if (!engine.Load(false).is_success) {
    ReportFailure(__FILE__, __LINE__, "engine failed to load Fixture A");
    return;
  }
  domain::PlanNode line_break_and_missing_post =
      InsertPlan("engagements", domain::Row{domain::Value(int64_t{105}),
                                            domain::Value(int64_t{999}),
                                            domain::Value(std::string("dave")),
                                            domain::Value(std::string("like")),
                                            domain::Value(std::string("a\nb")),
                                            domain::Value(int64_t{1005})});
  domain::PlanPtr duplicate_and_missing_post = MustBind(
      std::string(kInsertEngagement) + "(100, 999, 'dave', 'like', 'x', 1005)",
      engine.Catalog());
  if (!duplicate_and_missing_post) {
    return;
  }

  // Act
  ExecutionResult first = engine.Execute(line_break_and_missing_post);
  ExecutionResult second = engine.Execute(*duplicate_and_missing_post);

  // Assert
  ExpectError(first, ErrorKind::kUnsupported);
  ExpectErrorMessage(second, ErrorKind::kConstraintViolation,
                     "duplicate key: engagements.id=100");
}

TEST(test_engine_commit_failure_keeps_all_state) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto log = std::make_shared<RepositoryLog>();
  auto service = MakeLoadedService(dir.path(), log);
  if (!service) {
    return;
  }
  log->write_failure = "could not write users.csv (injected)";
  std::string memory_before = SnapshotTables(*service);

  // Act
  ExecutionResult update_result =
      ExecuteSql(*service, "UPDATE users SET username = 'alicia' WHERE id = 1");
  ExecutionResult delete_result =
      ExecuteSql(*service, "DELETE FROM users WHERE id = 4");

  // Assert
  ExpectErrorMessage(update_result, ErrorKind::kIo,
                     "could not write users.csv (injected)");
  ExpectErrorMessage(delete_result, ErrorKind::kIo,
                     "could not write users.csv (injected)");
  CHECK_EQ(log->write_calls, 2);
  CHECK_EQ(SnapshotTables(*service), memory_before);
}

TEST(test_engine_insert_append_failure_keeps_memory) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto log = std::make_shared<RepositoryLog>();
  auto service = MakeLoadedService(dir.path(), log);
  if (!service) {
    return;
  }
  log->append_failure = "could not append to engagements.csv (injected)";

  // Act
  ExecutionResult result =
      ExecuteSql(*service, std::string(kInsertEngagement) +
                               "(105, 12, 'dave', 'like', 'None', 1005)");

  // Assert
  ExpectErrorMessage(result, ErrorKind::kIo,
                     "could not append to engagements.csv (injected)");
  CHECK_EQ(log->append_calls, 1);
  CHECK_EQ(CountRows(*service, "engagements"), std::string("5"));
}

TEST(test_engine_update_plain_column_is_persisted) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto log = std::make_shared<RepositoryLog>();
  auto service = MakeLoadedService(dir.path(), log);
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result =
      ExecuteSql(*service, "UPDATE posts SET views = views + 1 WHERE id = 10");
  auto reloaded = MakeLoadedService(dir.path());
  if (!reloaded) {
    return;
  }

  // Assert
  ExpectAffected(result, 1);
  CHECK_EQ(Join(QueryLines(*reloaded, "SELECT views FROM posts WHERE id = 10")),
           std::string("6\n"));
  CHECK_EQ(log->write_calls, 1);
  if (log->written_table_names.size() != 1) {
    return;
  }
  CHECK_EQ(Join(log->written_table_names[0]), std::string("posts\n"));
}

TEST(test_engine_update_equal_value_keeps_stored_cell) {
  // Arrange: ABS(-0.0) is 0.0, which compares equal to the stored -0.0, while
  // the other row really changes, so the table is rewritten.
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }
  ExecuteSql(*service, "CREATE TABLE zeros (id INT PRIMARY KEY, v DOUBLE)");
  ExecuteSql(*service, "INSERT INTO zeros VALUES (1, -0.0), (2, -1.5)");

  // Act
  ExecutionResult result = ExecuteSql(*service, "UPDATE zeros SET v = ABS(v)");
  auto reloaded = MakeLoadedService(dir.path());
  if (!reloaded) {
    return;
  }

  // Assert
  ExpectAffected(result, 2);
  CHECK_EQ(Join(QueryLines(
               *reloaded, "SELECT id, CAST(v AS TEXT) FROM zeros ORDER BY id")),
           std::string("1\t-0.0\n2\t1.5\n"));
}

TEST(test_engine_update_referenced_column_cascades) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto log = std::make_shared<RepositoryLog>();
  auto service = MakeLoadedService(dir.path(), log);
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result =
      ExecuteSql(*service, "UPDATE users SET username = 'alicia' WHERE id = 1");
  auto reloaded = MakeLoadedService(dir.path());
  if (!reloaded) {
    return;
  }

  // Assert
  ExpectAffected(result, 1);
  CHECK_EQ(
      Join(QueryLines(*reloaded, "SELECT username FROM users WHERE id = 1")),
      std::string("alicia\n"));
  CHECK_EQ(
      Join(QueryLines(*reloaded, "SELECT id, username FROM posts ORDER BY id")),
      std::string("10\talicia\n11\talicia\n12\tbob\n"));
  CHECK_EQ(
      Join(QueryLines(*reloaded,
                      "SELECT id, username FROM engagements ORDER BY id")),
      std::string("100\tbob\n101\tcarol\n102\tbob\n103\talicia\n104\tcarol\n"));
  CHECK_EQ(log->write_calls, 1);
  if (log->written_table_names.size() != 1) {
    return;
  }
  CHECK_EQ(Join(log->written_table_names[0]),
           std::string("users\nposts\nengagements\n"));
}

TEST(test_engine_update_cascade_applies_old_to_new_map_once) {
  // Arrange
  TempDir dir;
  WriteFixtureC1(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result =
      ExecuteSql(*service, "UPDATE users SET username = location");
  auto reloaded = MakeLoadedService(dir.path());
  if (!reloaded) {
    return;
  }

  // Assert
  ExpectAffected(result, 2);
  CHECK_EQ(
      Join(QueryLines(*reloaded, "SELECT id, username FROM users ORDER BY id")),
      std::string("1\tb\n2\tc\n"));
  CHECK_EQ(
      Join(QueryLines(*reloaded, "SELECT id, username FROM posts ORDER BY id")),
      std::string("10\tb\n11\tc\n"));
}

TEST(test_engine_update_uniqueness_is_checked_on_final_state) {
  // Arrange: the swap is legal, the collision is not.
  TempDir swap_dir;
  WriteFixtureC2(swap_dir.path());
  auto swap_service = MakeLoadedService(swap_dir.path());
  if (!swap_service) {
    return;
  }
  TempDir collision_dir;
  WriteFixtureA(collision_dir.path());
  auto collision_service = MakeLoadedService(collision_dir.path());
  if (!collision_service) {
    return;
  }
  std::string files_before = ReadAllCsv(collision_dir.path());

  // Act
  ExecutionResult swap_result =
      ExecuteSql(*swap_service, "UPDATE users SET username = location");
  ExecutionResult collision_result = ExecuteSql(
      *collision_service, "UPDATE users SET username = 'bob' WHERE id = 1");
  auto reloaded = MakeLoadedService(swap_dir.path());
  if (!reloaded) {
    return;
  }

  // Assert
  ExpectAffected(swap_result, 2);
  CHECK_EQ(
      Join(QueryLines(*reloaded, "SELECT id, username FROM users ORDER BY id")),
      std::string("1\tb\n2\ta\n"));
  CHECK_EQ(
      Join(QueryLines(*reloaded, "SELECT id, username FROM posts ORDER BY id")),
      std::string("10\tb\n11\ta\n"));
  ExpectErrorMessage(collision_result, ErrorKind::kConstraintViolation,
                     "duplicate key: users.username=bob");
  CHECK_EQ(ReadAllCsv(collision_dir.path()), files_before);
}

TEST(test_engine_update_foreign_key_to_missing_value_violates_constraint) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }
  std::string files_before = ReadAllCsv(dir.path());

  // Act
  ExecutionResult result = ExecuteSql(
      *service, "UPDATE posts SET username = 'nobody' WHERE id = 10");

  // Assert
  ExpectErrorMessage(
      result, ErrorKind::kConstraintViolation,
      "no matching row: posts.username=nobody not found in users.username");
  CHECK_EQ(ReadAllCsv(dir.path()), files_before);
}

TEST(test_engine_update_assignments_use_old_row_and_skip_unchanged) {
  // Arrange
  TempDir swap_dir;
  WriteFixtureA(swap_dir.path());
  auto swap_service = MakeLoadedService(swap_dir.path());
  if (!swap_service) {
    return;
  }
  TempDir same_dir;
  WriteFixtureA(same_dir.path());
  auto same_log = std::make_shared<RepositoryLog>();
  auto same_service = MakeLoadedService(same_dir.path(), same_log);
  if (!same_service) {
    return;
  }

  // Act
  ExecutionResult swap_result = ExecuteSql(
      *swap_service,
      "UPDATE users SET username = location, location = username WHERE id = 4");
  ExecutionResult same_result = ExecuteSql(
      *same_service, "UPDATE users SET username = 'alice' WHERE id = 1");

  // Assert
  ExpectAffected(swap_result, 1);
  CHECK_EQ(
      Join(QueryLines(*swap_service,
                      "SELECT username, location FROM users WHERE id = 4")),
      std::string("Oslo\tdave\n"));
  ExpectAffected(same_result, 1);
  CHECK_EQ(same_log->write_calls, 0);
}

TEST(test_engine_update_row_selection_and_affected_rows) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult partial =
      ExecuteSql(*service, "UPDATE posts SET views = views + 1 WHERE id >= 11");
  ExecutionResult whole =
      ExecuteSql(*service, "UPDATE posts SET views = views + 1");
  std::vector<std::string> rows =
      QueryLines(*service, "SELECT id, views FROM posts ORDER BY id");

  // Assert
  ExpectAffected(partial, 2);
  ExpectAffected(whole, 3);
  CHECK_EQ(Join(rows), std::string("10\t6\n11\t2\n12\t9\n"));
}

TEST(test_engine_statements_matching_no_rows_affect_nothing) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto log = std::make_shared<RepositoryLog>();
  auto service = MakeLoadedService(dir.path(), log);
  if (!service) {
    return;
  }

  // Act
  ExecutionResult update_result =
      ExecuteSql(*service, "UPDATE posts SET views = 0 WHERE id = 999");
  ExecutionResult delete_result =
      ExecuteSql(*service, "DELETE FROM posts WHERE id = 999");

  // Assert
  ExpectAffected(update_result, 0);
  ExpectAffected(delete_result, 0);
  CHECK_EQ(log->write_calls, 0);
}

TEST(test_engine_delete_unreferenced_row_is_persisted) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result =
      ExecuteSql(*service, "DELETE FROM users WHERE id = 4");
  auto reloaded = MakeLoadedService(dir.path());
  if (!reloaded) {
    return;
  }

  // Assert
  ExpectAffected(result, 1);
  CHECK_EQ(CountRows(*reloaded, "users"), std::string("3"));
}

TEST(test_engine_delete_referenced_row_violates_constraint) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto log = std::make_shared<RepositoryLog>();
  auto service = MakeLoadedService(dir.path(), log);
  if (!service) {
    return;
  }
  std::string files_before = ReadAllCsv(dir.path());
  std::string memory_before = SnapshotTables(*service);

  // Act
  ExecutionResult referenced_by_posts =
      ExecuteSql(*service, "DELETE FROM users WHERE id = 3");
  ExecutionResult referenced_by_engagements =
      ExecuteSql(*service, "DELETE FROM users WHERE id = 2");
  ExecutionResult referenced_post =
      ExecuteSql(*service, "DELETE FROM posts WHERE id = 10");

  // Assert
  ExpectErrorMessage(referenced_by_posts, ErrorKind::kConstraintViolation,
                     "users.id=3 is referenced by posts.username");
  ExpectErrorMessage(referenced_by_engagements, ErrorKind::kConstraintViolation,
                     "users.id=2 is referenced by engagements.username");
  ExpectErrorMessage(referenced_post, ErrorKind::kConstraintViolation,
                     "posts.id=10 is referenced by engagements.postId");
  CHECK_EQ(ReadAllCsv(dir.path()), files_before);
  CHECK_EQ(SnapshotTables(*service), memory_before);
  CHECK_EQ(log->write_calls, 0);
}

TEST(test_engine_delete_checks_rows_in_table_order_and_is_all_or_nothing) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }
  std::string files_before = ReadAllCsv(dir.path());

  // Act
  ExecutionResult result =
      ExecuteSql(*service, "DELETE FROM users WHERE id >= 2");

  // Assert
  ExpectErrorMessage(result, ErrorKind::kConstraintViolation,
                     "users.id=2 is referenced by engagements.username");
  CHECK_EQ(
      Join(QueryLines(*service, "SELECT username FROM users WHERE id = 4")),
      std::string("dave\n"));
  CHECK_EQ(CountRows(*service, "users"), std::string("4"));
  CHECK_EQ(ReadAllCsv(dir.path()), files_before);
}

TEST(test_engine_delete_without_where_removes_all_rows) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result = ExecuteSql(*service, "DELETE FROM engagements");
  auto reloaded = MakeLoadedService(dir.path());
  if (!reloaded) {
    return;
  }

  // Assert
  ExpectAffected(result, 5);
  CHECK_EQ(CountRows(*service, "engagements"), std::string("0"));
  CHECK_EQ(CountRows(*reloaded, "engagements"), std::string("0"));
}

TEST(test_engine_load_reports_counts) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  infrastructure::FlatFileEngine serial_engine(MakeCsvRepository(dir.path()));
  infrastructure::FlatFileEngine parallel_engine(MakeCsvRepository(dir.path()));

  // Act
  infrastructure::LoadResult serial = serial_engine.Load(false);
  infrastructure::LoadResult parallel = parallel_engine.Load(true);

  // Assert
  ExpectLoadCounts(serial);
  ExpectLoadCounts(parallel);
}

TEST(test_engine_update_result_beyond_int32_is_persisted) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result = ExecuteSql(
      *service, "UPDATE posts SET views = views + 2147483647 WHERE id = 12");
  auto reloaded = MakeLoadedService(dir.path());
  if (!reloaded) {
    return;
  }

  // Assert
  ExpectAffected(result, 1);
  CHECK_EQ(Join(QueryLines(*reloaded, "SELECT views FROM posts WHERE id = 12")),
           std::string("2147483654\n"));
}

TEST(test_engine_update_does_not_revalidate_loaded_foreign_keys) {
  // Arrange: post 10 names alice, who is not in users after the load filter.
  TempDir dir;
  WriteFixtureB(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result =
      ExecuteSql(*service, "UPDATE posts SET views = 6 WHERE id = 10");

  // Assert
  ExpectAffected(result, 1);
  CHECK_EQ(Join(QueryLines(*service, "SELECT views FROM posts WHERE id = 10")),
           std::string("6\n"));
}

TEST(test_engine_update_unchanged_assignment_skips_uniqueness_check) {
  // Arrange: two users share a username, which the loader does not reject.
  TempDir dir;
  WriteCatalogFixture(dir.path());
  WriteFile(dir.path() / "users.csv",
            "id,username,location\n"
            "1,alice,Paris\n"
            "2,alice,Rome\n");
  WriteFile(dir.path() / "posts.csv", "id,content,username,views\n");
  WriteFile(dir.path() / "engagements.csv",
            "id,postid,username,type,comment,timestamp\n");
  auto log = std::make_shared<RepositoryLog>();
  auto service = MakeLoadedService(dir.path(), log);
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result =
      ExecuteSql(*service, "UPDATE users SET username = username WHERE id = 1");

  // Assert
  ExpectAffected(result, 1);
  CHECK_EQ(log->write_calls, 0);
}

TEST(test_engine_update_unchanged_foreign_key_is_not_revalidated) {
  // Arrange: post 10 names alice, who is not in users after the load filter.
  TempDir dir;
  WriteFixtureB(dir.path());
  auto log = std::make_shared<RepositoryLog>();
  auto service = MakeLoadedService(dir.path(), log);
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result = ExecuteSql(
      *service, "UPDATE posts SET username = username WHERE id = 10");

  // Assert
  ExpectAffected(result, 1);
  CHECK_EQ(log->write_calls, 0);
}

TEST(test_engine_write_statements_report_numeric_overflow) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult update_result = ExecuteSql(
      *service,
      "UPDATE posts SET views = views + 9223372036854775807 WHERE id = 12");
  ExecutionResult delete_result = ExecuteSql(
      *service, "DELETE FROM posts WHERE id + 9223372036854775807 > 0");
  std::vector<std::string> remaining =
      QueryLines(*service, "SELECT id, views FROM posts ORDER BY id");

  // Assert
  ExpectErrorMessage(update_result, ErrorKind::kRuntime, "numeric overflow");
  ExpectErrorMessage(delete_result, ErrorKind::kRuntime, "numeric overflow");
  CHECK_EQ(Join(remaining), std::string("10\t5\n11\t0\n12\t7\n"));
}

// ---- DDL, auto-increment, UNIQUE ------------------------------------------

namespace {

constexpr const char* kCreateUsers =
    "CREATE TABLE users (id INT PRIMARY KEY, username TEXT UNIQUE, location "
    "TEXT)";
constexpr const char* kCreatePosts =
    "CREATE TABLE posts (id INT PRIMARY KEY, content TEXT, username TEXT "
    "REFERENCES users(username), views INT)";
constexpr const char* kCreateEngagements =
    "CREATE TABLE engagements (id INT PRIMARY KEY, postId INT REFERENCES "
    "posts(id), username TEXT REFERENCES users(username), type TEXT, comment "
    "TEXT, timestamp INT)";
constexpr const char* kCreateJobs =
    "CREATE TABLE jobs (id INTEGER PRIMARY KEY AUTOINCREMENT, body TEXT)";

// An engine over dir that is loaded; null after reporting a failure.
std::unique_ptr<infrastructure::FlatFileEngine> LoadedEngine(
    const std::filesystem::path& dir) {
  auto engine =
      std::make_unique<infrastructure::FlatFileEngine>(MakeCsvRepository(dir));
  infrastructure::LoadResult loaded = engine->Load(false);
  if (!loaded.is_success) {
    ReportFailure(__FILE__, __LINE__, "engine failed to load: " + loaded.error);
    return nullptr;
  }
  return engine;
}

std::string TableNameList(const infrastructure::FlatFileEngine& engine) {
  return Join(engine.Catalog().TableNames());
}

}  // namespace

TEST(test_engine_create_table_persists_catalog_and_empty_csv) {
  // Arrange
  TempDir dir;
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult users = ExecuteSql(*service, kCreateUsers);
  ExecutionResult posts = ExecuteSql(
      *service,
      "CREATE TABLE posts (id INT PRIMARY KEY, content TEXT, username TEXT "
      "REFERENCES users(username), views INT DEFAULT 0)");
  auto engine = LoadedEngine(dir.path());
  if (!engine) {
    return;
  }
  std::optional<domain::TableSchema> reloaded_users =
      engine->Catalog().FindTable("users");
  std::optional<domain::TableSchema> reloaded_posts =
      engine->Catalog().FindTable("posts");

  // Assert
  ExpectComplete(users, "CREATE TABLE users");
  ExpectComplete(posts, "CREATE TABLE posts");
  CHECK_EQ(TableNameList(*engine), std::string("users\nposts\n"));
  CHECK(reloaded_users.has_value() && reloaded_posts.has_value());
  if (!reloaded_users || !reloaded_posts) {
    return;
  }
  CHECK_EQ(reloaded_users->unique_indexes, std::vector<std::size_t>({1}));
  CHECK_EQ(reloaded_posts->foreign_keys.size(), std::size_t{1});
  if (reloaded_posts->foreign_keys.size() == 1) {
    ExpectForeignKey(reloaded_posts->foreign_keys[0], 2, "users", 1);
  }
  CHECK(reloaded_posts->columns[3].default_value.has_value());
  CHECK_EQ(ReadFile(dir.path() / "users.csv"),
           std::string("id,username,location\n"));
  CHECK_EQ(ReadFile(dir.path() / "posts.csv"),
           std::string("id,content,username,views\n"));
}

TEST(test_engine_create_table_adopts_existing_csv) {
  // Arrange: Fixture A without its catalog.
  TempDir dir;
  WriteFixtureA(dir.path());
  std::filesystem::remove(dir.path() / "catalog.csv");
  const std::string files_before = ReadAllCsv(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult users = ExecuteSql(*service, kCreateUsers);
  ExecutionResult posts = ExecuteSql(*service, kCreatePosts);
  ExecutionResult engagements = ExecuteSql(*service, kCreateEngagements);

  // Assert
  ExpectComplete(users, "CREATE TABLE users (adopted 4 rows)");
  ExpectComplete(posts, "CREATE TABLE posts (adopted 3 rows)");
  ExpectComplete(engagements, "CREATE TABLE engagements (adopted 5 rows)");
  CHECK_EQ(ReadAllCsv(dir.path()), files_before);
  CHECK_EQ(CountRows(*service, "users"), std::string("4"));
}

TEST(test_engine_create_table_adoption_rejects_violating_rows) {
  // Arrange
  TempDir duplicate_key_dir;
  WriteFile(duplicate_key_dir.path() / "users.csv",
            "id,username,location\n1,a,x\n1,b,y\n");
  TempDir duplicate_unique_dir;
  WriteFile(duplicate_unique_dir.path() / "users.csv",
            "id,username,location\n1,a,x\n2,a,y\n");
  TempDir orphan_dir;
  WriteFile(orphan_dir.path() / "posts.csv",
            "id,content,username,views\n10,x,nobody,0\n");
  auto duplicate_key = MakeLoadedService(duplicate_key_dir.path());
  auto duplicate_unique = MakeLoadedService(duplicate_unique_dir.path());
  auto orphan = MakeLoadedService(orphan_dir.path());
  if (!duplicate_key || !duplicate_unique || !orphan) {
    return;
  }
  ExecuteSql(*orphan, kCreateUsers);

  // Act
  ExecutionResult key_result = ExecuteSql(*duplicate_key, kCreateUsers);
  ExecutionResult unique_result = ExecuteSql(*duplicate_unique, kCreateUsers);
  ExecutionResult orphan_result = ExecuteSql(*orphan, kCreatePosts);

  // Assert
  ExpectErrorMessage(key_result, ErrorKind::kConstraintViolation,
                     "cannot adopt users.csv: duplicate key: users.id=1");
  ExpectErrorMessage(unique_result, ErrorKind::kConstraintViolation,
                     "cannot adopt users.csv: duplicate key: users.username=a");
  ExpectErrorMessage(orphan_result, ErrorKind::kConstraintViolation,
                     "cannot adopt posts.csv: no matching row: "
                     "posts.username=nobody not found in users.username");
  CHECK(!std::filesystem::exists(duplicate_key_dir.path() / "catalog.csv"));
  CHECK(!std::filesystem::exists(duplicate_unique_dir.path() / "catalog.csv"));
  CHECK_EQ(Join(QueryLines(*orphan, "SELECT COUNT(*) FROM users")),
           std::string("0\n"));
  CHECK(AsError(ExecuteSql(*orphan, "SELECT id FROM posts")) != nullptr);
}

TEST(test_engine_create_table_adoption_rejects_header_mismatch) {
  // Arrange
  TempDir dir;
  WriteFile(dir.path() / "users.csv", "id,name\n1,a\n");
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result = ExecuteSql(*service, kCreateUsers);

  // Assert
  ExpectErrorMessage(result, ErrorKind::kIo,
                     "header of users.csv does not match the declared columns");
  CHECK(!std::filesystem::exists(dir.path() / "catalog.csv"));
}

TEST(test_engine_add_column_fills_default_and_persists) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result =
      ExecuteSql(*service, "ALTER TABLE users ADD COLUMN age INT DEFAULT 7");
  auto reloaded = MakeLoadedService(dir.path());
  if (!reloaded) {
    return;
  }

  // Assert
  ExpectComplete(result, "ALTER TABLE users");
  CHECK_EQ(Join(QueryLines(*reloaded, "SELECT id, age FROM users ORDER BY id")),
           std::string("1\t7\n2\t7\n3\t7\n4\t7\n"));
  CHECK_EQ(ReadFile(dir.path() / "users.csv"),
           std::string("id,username,location,age\n"
                       "1,alice,Paris,7\n2,carol,Paris,7\n"
                       "3,bob,Rome,7\n4,dave,Oslo,7\n"));
  CHECK(
      ReadFile(dir.path() / "catalog.csv").find("users,3,age,INT,0,0,0,1,7") !=
      std::string::npos);
}

TEST(test_engine_drop_table_removes_file_and_catalog_entry) {
  // Arrange
  TempDir dir;
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }
  ExecuteSql(*service, kCreateJobs);
  ExecuteSql(*service, "INSERT INTO jobs (body) VALUES ('a')");
  CHECK(std::filesystem::exists(dir.path() / "jobs.csv"));

  // Act
  ExecutionResult result = ExecuteSql(*service, "DROP TABLE jobs");
  auto engine = LoadedEngine(dir.path());
  if (!engine) {
    return;
  }

  // Assert
  ExpectComplete(result, "DROP TABLE jobs");
  CHECK(!std::filesystem::exists(dir.path() / "jobs.csv"));
  CHECK_EQ(TableNameList(*engine), std::string(""));
}

TEST(test_engine_drop_referenced_table_is_rejected) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }
  const std::string files_before =
      ReadAllCsv(dir.path()) + ReadFile(dir.path() / "catalog.csv");

  // Act
  ExecutionResult result = ExecuteSql(*service, "DROP TABLE users");

  // Assert
  ExpectErrorMessage(result, ErrorKind::kBind,
                     "table users is referenced by posts.username");
  CHECK_EQ(ReadAllCsv(dir.path()) + ReadFile(dir.path() / "catalog.csv"),
           files_before);
}

TEST(test_engine_ddl_commit_failure_keeps_catalog_and_tables) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto log = std::make_shared<RepositoryLog>();
  auto service = MakeLoadedService(dir.path(), log);
  if (!service) {
    return;
  }
  log->write_failure = "could not write catalog.csv (injected)";
  std::string memory_before = SnapshotTables(*service);
  const std::string files_before =
      ReadAllCsv(dir.path()) + ReadFile(dir.path() / "catalog.csv");

  // Act
  ExecutionResult create_result = ExecuteSql(*service, kCreateJobs);
  ExecutionResult alter_result =
      ExecuteSql(*service, "ALTER TABLE users ADD COLUMN age INT DEFAULT 0");
  ExecutionResult drop_result = ExecuteSql(*service, "DROP TABLE engagements");

  // Assert
  for (const ExecutionResult* result :
       {&create_result, &alter_result, &drop_result}) {
    ExpectErrorMessage(*result, ErrorKind::kIo,
                       "could not write catalog.csv (injected)");
  }
  CHECK_EQ(SnapshotTables(*service), memory_before);
  CHECK(AsError(ExecuteSql(*service, "SELECT id FROM jobs")) != nullptr);
  CHECK(AsError(ExecuteSql(*service, "SELECT age FROM users")) != nullptr);
  CHECK_EQ(ReadAllCsv(dir.path()) + ReadFile(dir.path() / "catalog.csv"),
           files_before);
}

TEST(test_engine_reopen_after_ddl_reads_same_schema) {
  // Arrange
  TempDir dir;
  {
    auto first = MakeLoadedService(dir.path());
    if (!first) {
      return;
    }
    ExecuteSql(*first,
               "CREATE TABLE owners (id INT PRIMARY KEY, name TEXT UNIQUE)");
    ExecuteSql(*first,
               "CREATE TABLE jobs (id INTEGER PRIMARY KEY AUTOINCREMENT, title "
               "TEXT UNIQUE, owner TEXT REFERENCES owners(name), score DOUBLE "
               "DEFAULT 0.5)");
    ExecuteSql(*first, "INSERT INTO owners VALUES (1, 'ann')");
    ExecuteSql(*first, "INSERT INTO jobs (title, owner) VALUES ('a', 'ann')");
    ExecuteSql(*first, "INSERT INTO jobs (title, owner) VALUES ('b', 'ann')");
  }
  auto second = MakeLoadedService(dir.path());
  if (!second) {
    return;
  }

  // Act
  ExecutionResult next = ExecuteSql(
      *second, "INSERT INTO jobs (title, owner) VALUES ('c', 'ann')");
  ExecutionResult duplicate_title = ExecuteSql(
      *second, "INSERT INTO jobs (title, owner) VALUES ('a', 'ann')");
  ExecutionResult unknown_owner = ExecuteSql(
      *second, "INSERT INTO jobs (title, owner) VALUES ('d', 'zed')");

  // Assert
  ExpectAffected(next, 1);
  ExpectErrorMessage(duplicate_title, ErrorKind::kConstraintViolation,
                     "duplicate key: jobs.title=a");
  ExpectErrorMessage(
      unknown_owner, ErrorKind::kConstraintViolation,
      "no matching row: jobs.owner=zed not found in owners.name");
  CHECK_EQ(Join(QueryLines(*second, "SELECT id, score FROM jobs ORDER BY id")),
           std::string("1\t0.5\n2\t0.5\n3\t0.5\n"));
}

TEST(test_engine_insert_omitted_auto_increment_uses_next_value) {
  // Arrange
  TempDir dir;
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }
  ExecuteSql(*service, kCreateJobs);

  // Act
  ExecuteSql(*service, "INSERT INTO jobs (body) VALUES ('a')");
  ExecuteSql(*service, "INSERT INTO jobs (body) VALUES ('b')");
  ExecuteSql(*service, "INSERT INTO jobs (id, body) VALUES (10, 'c')");
  ExecuteSql(*service, "INSERT INTO jobs (body) VALUES ('d')");
  ExecuteSql(*service, "DELETE FROM jobs WHERE id = 11");
  ExecuteSql(*service, "INSERT INTO jobs (body) VALUES ('e')");
  ExecuteSql(*service,
             "INSERT INTO jobs (id, body) VALUES (9223372036854775807, 'max')");
  const std::string file_before = ReadFile(dir.path() / "jobs.csv");
  ExecutionResult overflow =
      ExecuteSql(*service, "INSERT INTO jobs (body) VALUES ('over')");

  // Assert
  CHECK_EQ(Join(QueryLines(*service, "SELECT id, body FROM jobs ORDER BY id")),
           std::string("1\ta\n2\tb\n10\tc\n11\te\n9223372036854775807\tmax\n"));
  ExpectErrorMessage(overflow, ErrorKind::kRuntime, "numeric overflow");
  CHECK_EQ(ReadFile(dir.path() / "jobs.csv"), file_before);
}

TEST(test_engine_insert_omitted_default_column_uses_default) {
  // Arrange
  TempDir dir;
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }
  ExecuteSql(*service,
             "CREATE TABLE notes (id INT PRIMARY KEY, body TEXT, owner TEXT "
             "DEFAULT 'none', score DOUBLE DEFAULT 1)");

  // Act
  ExecutionResult result =
      ExecuteSql(*service, "INSERT INTO notes (id, body) VALUES (1, 'a')");
  auto reloaded = MakeLoadedService(dir.path());
  if (!reloaded) {
    return;
  }

  // Assert
  ExpectAffected(result, 1);
  CHECK_EQ(Join(QueryLines(*reloaded, "SELECT owner, score FROM notes")),
           std::string("none\t1.0\n"));
  CHECK_EQ(ReadFile(dir.path() / "notes.csv"),
           std::string("id,body,owner,score\n1,a,none,1.0\n"));
}

TEST(test_engine_insert_duplicate_unique_value_violates_constraint) {
  // Arrange
  TempDir dir;
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }
  ExecuteSql(*service,
             "CREATE TABLE contacts (id INT PRIMARY KEY, email TEXT UNIQUE)");
  ExecuteSql(*service, "INSERT INTO contacts VALUES (1, 'a@x')");
  const std::string file_before = ReadFile(dir.path() / "contacts.csv");

  // Act
  ExecutionResult result =
      ExecuteSql(*service, "INSERT INTO contacts VALUES (2, 'a@x')");

  // Assert
  ExpectErrorMessage(result, ErrorKind::kConstraintViolation,
                     "duplicate key: contacts.email=a@x");
  CHECK_EQ(ReadFile(dir.path() / "contacts.csv"), file_before);
}

TEST(test_engine_update_unique_column_is_checked_on_final_state) {
  // Arrange
  TempDir dir;
  auto log = std::make_shared<RepositoryLog>();
  auto service = MakeLoadedService(dir.path(), log);
  if (!service) {
    return;
  }
  ExecuteSql(*service,
             "CREATE TABLE pairs (id INT PRIMARY KEY, a TEXT UNIQUE, b TEXT)");
  ExecuteSql(*service, "INSERT INTO pairs VALUES (1, 'x', 'y')");
  ExecuteSql(*service, "INSERT INTO pairs VALUES (2, 'y', 'x')");

  // Act
  ExecutionResult swap = ExecuteSql(*service, "UPDATE pairs SET a = b");
  ExecutionResult collide =
      ExecuteSql(*service, "UPDATE pairs SET a = 'x' WHERE id = 1");
  const int writes_before = log->write_calls;
  ExecutionResult unchanged =
      ExecuteSql(*service, "UPDATE pairs SET a = a WHERE id = 1");

  // Assert
  ExpectAffected(swap, 2);
  ExpectErrorMessage(collide, ErrorKind::kConstraintViolation,
                     "duplicate key: pairs.a=x");
  ExpectAffected(unchanged, 1);
  CHECK_EQ(log->write_calls, writes_before);
  CHECK_EQ(Join(QueryLines(*service, "SELECT id, a FROM pairs ORDER BY id")),
           std::string("1\ty\n2\tx\n"));
}

TEST(test_engine_insert_text_with_comma_and_quote_is_persisted) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result = ExecuteSql(
      *service,
      "INSERT INTO posts (id, content, username, views) VALUES (13, 'Hello, "
      "\"world\"', 'alice', 1)");
  auto reloaded = MakeLoadedService(dir.path());
  if (!reloaded) {
    return;
  }

  // Assert
  ExpectAffected(result, 1);
  CHECK_EQ(
      Join(QueryLines(*reloaded, "SELECT content FROM posts WHERE id = 13")),
      std::string("Hello, \"world\"\n"));
  CHECK(ReadFile(dir.path() / "posts.csv")
            .find("13,\"Hello, \"\"world\"\"\",alice,1\n") !=
        std::string::npos);
}

// ---- LIMIT, aggregates, LIKE, functions, joins, multi-row INSERT -----------

TEST(test_engine_limit_and_offset_select_ordered_window) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }
  const std::string base = "SELECT id FROM users ORDER BY id";

  // Act and Assert
  CHECK_EQ(Join(QueryLines(*service, base + " LIMIT 2")),
           std::string("1\n2\n"));
  CHECK_EQ(Join(QueryLines(*service, base + " LIMIT 2 OFFSET 1")),
           std::string("2\n3\n"));
  CHECK_EQ(Join(QueryLines(*service, base + " LIMIT 0")), std::string());
  CHECK_EQ(Join(QueryLines(*service, base + " LIMIT 5 OFFSET 10")),
           std::string());
  CHECK_EQ(Join(QueryLines(*service, base + " LIMIT 100")),
           std::string("1\n2\n3\n4\n"));
  CHECK_EQ(Join(QueryLines(*service,
                           "SELECT id FROM users ORDER BY id DESC LIMIT 1")),
           std::string("4\n"));
  CHECK_EQ(Join(QueryLines(*service,
                           "SELECT username, COUNT(*) FROM posts GROUP BY "
                           "username ORDER BY username LIMIT 1")),
           std::string("alice\t2\n"));
  ExecutionResult empty = ExecuteSql(*service, base + " LIMIT 0");
  const domain::ResultSet* empty_set = AsResultSet(empty);
  CHECK(empty_set != nullptr);
  if (empty_set != nullptr) {
    CHECK_EQ(empty_set->columns.size(), std::size_t{1});
    CHECK_EQ(empty_set->rows.size(), std::size_t{0});
  }
}

TEST(test_engine_avg_min_max_over_rows_and_groups) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act and Assert
  CHECK_EQ(
      Join(QueryLines(*service,
                      "SELECT AVG(views), MIN(views), MAX(views) FROM posts")),
      std::string("4.0\t0\t7\n"));
  CHECK_EQ(Join(QueryLines(*service,
                           "SELECT MIN(username), MAX(username) FROM users")),
           std::string("alice\tdave\n"));
  CHECK_EQ(Join(QueryLines(*service,
                           "SELECT username, AVG(views), MIN(views), "
                           "MAX(views) FROM posts GROUP BY username ORDER BY "
                           "username")),
           std::string("alice\t2.5\t0\t5\nbob\t7.0\t7\t7\n"));
}

TEST(test_engine_avg_min_max_on_empty_input_report_runtime_error) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  ExecutionResult average =
      ExecuteSql(*service, "SELECT AVG(views) FROM posts WHERE id = 999");
  ExecutionResult minimum =
      ExecuteSql(*service, "SELECT MIN(views) FROM posts WHERE id = 999");
  ExecutionResult maximum =
      ExecuteSql(*service, "SELECT MAX(username) FROM users WHERE id = 999");

  // Assert
  ExpectErrorMessage(average, ErrorKind::kRuntime, "AVG over no rows");
  ExpectErrorMessage(minimum, ErrorKind::kRuntime, "MIN over no rows");
  ExpectErrorMessage(maximum, ErrorKind::kRuntime, "MAX over no rows");
  CHECK_EQ(Join(QueryLines(*service,
                           "SELECT COUNT(*), SUM(views) FROM posts WHERE id = "
                           "999")),
           std::string("0\t0\n"));
  CHECK_EQ(Join(QueryLines(*service,
                           "SELECT username, AVG(views) FROM posts WHERE id = "
                           "999 GROUP BY username")),
           std::string());
}

TEST(test_engine_like_filters_rows) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act and Assert
  CHECK_EQ(
      Join(QueryLines(*service,
                      "SELECT username FROM users WHERE username LIKE 'a%'")),
      std::string("alice\n"));
  CHECK_EQ(Join(QueryLines(*service,
                           "SELECT username FROM users WHERE username LIKE "
                           "'%o%' ORDER BY username")),
           std::string("bob\ncarol\n"));
  CHECK_EQ(
      Join(QueryLines(*service,
                      "SELECT username FROM users WHERE username LIKE '_ob'")),
      std::string("bob\n"));
  CHECK_EQ(Join(QueryLines(*service,
                           "SELECT username FROM users WHERE username LIKE "
                           "'ALICE'")),
           std::string("alice\n"));
  CHECK_EQ(Join(QueryLines(
               *service, "SELECT COUNT(*) FROM users WHERE username LIKE '%'")),
           std::string("4\n"));
  CHECK_EQ(Join(QueryLines(
               *service, "SELECT username FROM users WHERE username LIKE ''")),
           std::string());
  CHECK_EQ(Join(QueryLines(*service,
                           "SELECT p.id FROM posts p JOIN users u ON "
                           "p.username = u.username WHERE u.username LIKE "
                           "'%ice' AND p.content LIKE '%post%'")),
           std::string("11\n"));
}

TEST(test_engine_functions_apply_in_every_clause) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act and Assert
  CHECK_EQ(Join(QueryLines(*service,
                           "SELECT UPPER(username), LENGTH(location) FROM "
                           "users WHERE id = 1")),
           std::string("ALICE\t5\n"));
  CHECK_EQ(Join(QueryLines(*service,
                           "SELECT id FROM users WHERE LENGTH(username) = 3")),
           std::string("3\n"));
  CHECK_EQ(Join(QueryLines(*service,
                           "SELECT id, LOWER(username) FROM users ORDER BY "
                           "LOWER(username) DESC LIMIT 1")),
           std::string("4\tdave\n"));
  CHECK_EQ(Join(QueryLines(*service,
                           "SELECT UPPER(location), COUNT(*) FROM users GROUP "
                           "BY UPPER(location) ORDER BY 1")),
           std::string("OSLO\t1\nPARIS\t2\nROME\t1\n"));
  CHECK_EQ(Join(QueryLines(*service,
                           "SELECT username || '@' || location FROM users "
                           "WHERE id = 3")),
           std::string("bob@Rome\n"));

  ExecutionResult update = ExecuteSql(
      *service, "UPDATE users SET location = UPPER(location) WHERE id = 1");
  auto reloaded = MakeLoadedService(dir.path());
  if (!reloaded) {
    return;
  }
  ExpectAffected(update, 1);
  CHECK_EQ(
      Join(QueryLines(*reloaded, "SELECT location FROM users WHERE id = 1")),
      std::string("PARIS\n"));
  CHECK(ReadFile(dir.path() / "users.csv").find("\n1,alice,PARIS\n") !=
        std::string::npos);
}

TEST(test_engine_arithmetic_and_cast_in_queries) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  std::vector<std::string> rows = QueryLines(
      *service,
      "SELECT views * 2, views / 2, views / 2.0, -views * 3, CAST(views AS "
      "TEXT) || '!', CAST('12' AS INT) + 1, ROUND(views / 3.0, 2) FROM posts "
      "WHERE id = 10");

  // Assert
  CHECK_EQ(Join(rows), std::string("10\t2\t2.5\t-15\t5!\t13\t1.67\n"));
}

TEST(test_engine_runtime_errors_surface_as_kruntime) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }
  const std::string tables_before = SnapshotTables(*service);
  const std::string files_before = ReadAllCsv(dir.path());

  // Act and Assert: queries
  ExpectErrorMessage(
      ExecuteSql(*service, "SELECT 1 / (views - views) FROM posts"),
      ErrorKind::kRuntime, "division by zero");
  ExpectErrorMessage(
      ExecuteSql(*service, "SELECT CAST(username AS INT) FROM users"),
      ErrorKind::kRuntime, "cannot cast 'alice' to INT");
  ExpectErrorMessage(
      ExecuteSql(*service, "SELECT SUBSTR(username, 1, -1) FROM users"),
      ErrorKind::kRuntime, "negative substring length");
  ExpectErrorMessage(ExecuteSql(*service, "SELECT ROUND(views, 16) FROM posts"),
                     ErrorKind::kRuntime, "ROUND digits out of range");
  ExpectErrorMessage(
      ExecuteSql(*service,
                 "SELECT views * 9223372036854775807 FROM posts WHERE id = 10"),
      ErrorKind::kRuntime, "numeric overflow");

  // Act and Assert: writes leave memory and files unchanged
  ExpectErrorMessage(ExecuteSql(*service, "UPDATE posts SET views = views / 0"),
                     ErrorKind::kRuntime, "division by zero");
  ExpectErrorMessage(ExecuteSql(*service, "DELETE FROM users WHERE 1 / 0 = 1"),
                     ErrorKind::kRuntime, "division by zero");
  ExpectErrorMessage(
      ExecuteSql(*service,
                 "INSERT INTO posts (id, content, username, views) VALUES (13, "
                 "'x', 'alice', 1 / 0)"),
      ErrorKind::kRuntime, "division by zero");
  CHECK_EQ(SnapshotTables(*service), tables_before);
  CHECK_EQ(ReadAllCsv(dir.path()), files_before);
}

TEST(test_engine_three_table_join_aggregates_over_chain) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  std::vector<std::string> rows = QueryLines(
      *service,
      "SELECT u.username, COUNT(*) FROM users u JOIN posts p ON p.username = "
      "u.username JOIN engagements e ON e.postId = p.id GROUP BY u.username "
      "ORDER BY u.username");

  // Assert
  CHECK_EQ(Join(rows), std::string("alice\t3\nbob\t2\n"));
}

TEST(test_engine_four_table_join_and_non_equi_steps_return_rows) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  std::vector<std::string> four_tables = QueryLines(
      *service,
      "SELECT u1.id, u2.id, e.id FROM users u1 JOIN posts p ON p.username = "
      "u1.username JOIN engagements e ON e.postId = p.id JOIN users u2 ON "
      "e.username = u2.username ORDER BY u1.id, u2.id, e.id");
  std::vector<std::string> non_equi = QueryLines(
      *service,
      "SELECT a.id, b.id, c.id FROM users a JOIN users b ON b.id > a.id JOIN "
      "users c ON c.id > b.id WHERE a.id = 1 ORDER BY b.id, c.id");

  // Assert
  CHECK_EQ(
      Join(four_tables),
      std::string("1\t2\t101\n1\t3\t100\n1\t3\t102\n3\t1\t103\n3\t2\t104\n"));
  CHECK_EQ(Join(non_equi), std::string("1\t2\t3\n1\t2\t4\n1\t3\t4\n"));
}

TEST(test_engine_join_key_may_come_from_any_left_table) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act
  std::vector<std::string> rows = QueryLines(
      *service,
      "SELECT p.id, e.id FROM users u JOIN posts p ON p.username = u.username "
      "JOIN engagements e ON e.username = u.username ORDER BY p.id, e.id");

  // Assert
  CHECK_EQ(Join(rows), std::string("10\t103\n11\t103\n12\t100\n12\t102\n"));
}

TEST(test_engine_multi_row_insert_appends_all_rows_once) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto log = std::make_shared<RepositoryLog>();
  auto service = MakeLoadedService(dir.path(), log);
  if (!service) {
    return;
  }

  // Act
  ExecutionResult result = ExecuteSql(
      *service,
      "INSERT INTO users (id, username, location) VALUES (5, 'eve', 'X'), (6, "
      "'frank', 'Y, Z'), (7, 'gina', 'say \"hi\"')");
  auto reloaded = MakeLoadedService(dir.path());
  if (!reloaded) {
    return;
  }

  // Assert
  ExpectAffected(result, 3);
  CHECK_EQ(log->append_calls, 1);
  CHECK_EQ(log->write_calls, 0);
  CHECK_EQ(CountRows(*reloaded, "users"), std::string("7"));
  CHECK_EQ(Join(QueryLines(*reloaded,
                           "SELECT id, username, location FROM users WHERE id "
                           ">= 5 ORDER BY id")),
           std::string("5\teve\tX\n6\tfrank\tY, Z\n7\tgina\tsay \"hi\"\n"));
  const std::string file = ReadFile(dir.path() / "users.csv");
  CHECK(file.size() >= 1 &&
        file.find("5,eve,X\n6,frank,\"Y, Z\"\n7,gina,\"say \"\"hi\"\"\"\n") !=
            std::string::npos);
}

TEST(test_engine_multi_row_insert_fails_as_a_whole) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto log = std::make_shared<RepositoryLog>();
  auto service = MakeLoadedService(dir.path(), log);
  if (!service) {
    return;
  }
  const std::string tables_before = SnapshotTables(*service);
  const std::string files_before = ReadAllCsv(dir.path());
  const std::string users =
      "INSERT INTO users (id, username, location) VALUES ";

  // Act and Assert
  ExpectErrorMessage(
      ExecuteSql(*service, users + "(5, 'eve', 'X'), (1, 'dup', 'Y')"),
      ErrorKind::kConstraintViolation, "duplicate key: users.id=1");
  ExpectErrorMessage(
      ExecuteSql(*service, users + "(5, 'eve', 'X'), (5, 'frank', 'Y')"),
      ErrorKind::kConstraintViolation, "duplicate key: users.id=5");
  ExpectErrorMessage(
      ExecuteSql(*service, users + "(5, 'eve', 'X'), (6, 'eve', 'Y')"),
      ErrorKind::kConstraintViolation, "duplicate key: users.username=eve");
  ExpectErrorMessage(
      ExecuteSql(*service,
                 "INSERT INTO posts (id, content, username, views) VALUES (20, "
                 "'a', 'alice', 1), (21, 'b', 'bob', 1), (22, 'c', 'nobody', "
                 "1)"),
      ErrorKind::kConstraintViolation,
      "no matching row: posts.username=nobody not found in users.username");
  ExpectErrorMessage(
      ExecuteSql(*service, users + "(5, 'eve', 'X'), (6, 'a\nb', 'Y')"),
      ErrorKind::kUnsupported,
      "flatfile engine cannot store a line break in users.username");

  CHECK_EQ(SnapshotTables(*service), tables_before);
  CHECK_EQ(ReadAllCsv(dir.path()), files_before);
  CHECK_EQ(log->append_calls, 0);
}

TEST(test_engine_multi_row_insert_assigns_increasing_autoincrement) {
  // Arrange
  TempDir dir;
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }
  ExecuteSql(*service, kCreateJobs);

  // Act
  ExecutionResult first = ExecuteSql(
      *service, "INSERT INTO jobs (body) VALUES ('a'), ('b'), ('c')");
  ExecuteSql(*service, "DELETE FROM jobs WHERE id = 3");
  ExecutionResult second =
      ExecuteSql(*service, "INSERT INTO jobs (body) VALUES ('d'), ('e')");
  ExecuteSql(
      *service,
      "INSERT INTO jobs (id, body) VALUES (9223372036854775806, 'near')");
  const std::string file_before = ReadFile(dir.path() / "jobs.csv");
  ExecutionResult overflow =
      ExecuteSql(*service, "INSERT INTO jobs (body) VALUES ('x'), ('y')");

  // Assert
  ExpectAffected(first, 3);
  ExpectAffected(second, 2);
  CHECK_EQ(Join(QueryLines(*service, "SELECT id, body FROM jobs ORDER BY id")),
           std::string("1\ta\n2\tb\n3\td\n4\te\n9223372036854775806\tnear\n"));
  ExpectErrorMessage(overflow, ErrorKind::kRuntime, "numeric overflow");
  CHECK_EQ(ReadFile(dir.path() / "jobs.csv"), file_before);
}

TEST(test_engine_multi_row_insert_write_failure_keeps_memory) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto log = std::make_shared<RepositoryLog>();
  auto service = MakeLoadedService(dir.path(), log);
  if (!service) {
    return;
  }
  const std::string files_before = ReadAllCsv(dir.path());
  log->append_failure = "could not append to users.csv (injected)";

  // Act
  ExecutionResult result = ExecuteSql(
      *service,
      "INSERT INTO users (id, username, location) VALUES (5, 'a', 'x'), (6, "
      "'b', 'y'), (7, 'c', 'z')");

  // Assert
  ExpectErrorMessage(result, ErrorKind::kIo,
                     "could not append to users.csv (injected)");
  CHECK_EQ(log->append_calls, 1);
  CHECK_EQ(CountRows(*service, "users"), std::string("4"));
  CHECK_EQ(ReadAllCsv(dir.path()), files_before);
}

TEST(test_engine_multi_row_insert_applies_defaults_and_quotes_text) {
  // Arrange
  TempDir dir;
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }
  ExecuteSql(*service,
             "CREATE TABLE d (id INT PRIMARY KEY, body TEXT, owner TEXT "
             "DEFAULT 'none')");

  // Act
  ExecutionResult result = ExecuteSql(
      *service,
      "INSERT INTO d (id, body) VALUES (1, 'a, b'), (2, 'say \"hi\"')");
  auto reloaded = MakeLoadedService(dir.path());
  if (!reloaded) {
    return;
  }

  // Assert
  ExpectAffected(result, 2);
  CHECK_EQ(
      Join(QueryLines(*reloaded, "SELECT id, body, owner FROM d ORDER BY id")),
      std::string("1\ta, b\tnone\n2\tsay \"hi\"\tnone\n"));
  CHECK_EQ(ReadFile(dir.path() / "d.csv"),
           std::string("id,body,owner\n1,\"a, b\",none\n2,\"say \"\"hi\"\"\","
                       "none\n"));
}

TEST(test_engine_write_refuses_when_a_file_changed_on_disk) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }
  const std::string tables_before = SnapshotTables(*service);
  const std::filesystem::path users_path = dir.path() / "users.csv";
  const std::string edited = ReadFile(users_path) + "9,external,Nowhere\n";
  WriteFile(users_path, edited);
  std::filesystem::last_write_time(
      users_path,
      std::filesystem::last_write_time(users_path) + std::chrono::seconds(2));
  const std::string changed_message =
      "users.csv changed on disk since it was loaded; run .open again";

  // Act
  ExecutionResult update =
      ExecuteSql(*service, "UPDATE users SET location = 'X' WHERE id = 1");
  ExecutionResult insert = ExecuteSql(
      *service,
      "INSERT INTO users (id, username, location) VALUES (50, 'zed', 'Rome')");
  ExecutionResult unrelated =
      ExecuteSql(*service, "CREATE TABLE extra (id INT PRIMARY KEY)");
  ExecutionResult read = ExecuteSql(*service, "SELECT COUNT(*) FROM users");

  // Assert: the writes that touch users.csv are refused, nothing is written
  ExpectErrorMessage(update, ErrorKind::kIo, changed_message);
  ExpectErrorMessage(insert, ErrorKind::kIo, changed_message);
  CHECK_EQ(ReadFile(users_path), edited);
  CHECK_EQ(SnapshotTables(*service), tables_before);
  CHECK(AsResultSet(read) != nullptr);
  // A statement that writes only other files is not affected.
  ExpectComplete(unrelated, "CREATE TABLE extra");

  // Opening again picks up the external edit and writes work again.
  auto reloaded = MakeLoadedService(dir.path());
  if (!reloaded) {
    return;
  }
  CHECK_EQ(CountRows(*reloaded, "users"), std::string("5"));
  ExpectAffected(
      ExecuteSql(*reloaded, "UPDATE users SET location = 'X' WHERE id = 1"), 1);
}

TEST(test_engine_write_refuses_when_only_size_or_only_mtime_changed) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }
  const std::filesystem::path users_path = dir.path() / "users.csv";
  const std::string loaded_text = ReadFile(users_path);
  const auto loaded_time = std::filesystem::last_write_time(users_path);
  const std::string changed_message =
      "users.csv changed on disk since it was loaded; run .open again";
  const char* const update = "UPDATE users SET location = 'X' WHERE id = 1";

  // Act: the same bytes with a later modification time
  std::filesystem::last_write_time(users_path,
                                   loaded_time + std::chrono::seconds(2));
  ExecutionResult only_time = ExecuteSql(*service, update);
  // Act: other bytes of another size with the recorded modification time
  const std::string longer_text = loaded_text + "\n";
  WriteFile(users_path, longer_text);
  std::filesystem::last_write_time(users_path, loaded_time);
  ExecutionResult only_size = ExecuteSql(*service, update);

  // Assert
  ExpectErrorMessage(only_time, ErrorKind::kIo, changed_message);
  ExpectErrorMessage(only_size, ErrorKind::kIo, changed_message);
  CHECK_EQ(ReadFile(users_path), longer_text);
}

TEST(test_engine_consecutive_writes_do_not_trip_the_change_check) {
  // Arrange
  TempDir dir;
  WriteFixtureA(dir.path());
  auto service = MakeLoadedService(dir.path());
  if (!service) {
    return;
  }

  // Act and Assert: every kind of write, one after the other
  ExpectAffected(
      ExecuteSql(*service,
                 "INSERT INTO users (id, username, location) VALUES (5, 'eve', "
                 "'X')"),
      1);
  ExpectAffected(
      ExecuteSql(*service, "UPDATE users SET location = 'Y' WHERE id = 5"), 1);
  ExpectAffected(ExecuteSql(*service, "DELETE FROM users WHERE id = 5"), 1);
  ExpectComplete(
      ExecuteSql(*service, "ALTER TABLE users ADD COLUMN age INT DEFAULT 0"),
      "ALTER TABLE users");
  ExpectAffected(
      ExecuteSql(*service,
                 "INSERT INTO users (id, username, location) VALUES (6, "
                 "'frank', 'Z')"),
      1);
  ExpectComplete(ExecuteSql(*service, "DROP TABLE engagements"),
                 "DROP TABLE engagements");
  ExpectAffected(
      ExecuteSql(*service,
                 "INSERT INTO users (id, username, location) VALUES (7, "
                 "'gina', 'W')"),
      1);
}

}  // namespace strata::testing
