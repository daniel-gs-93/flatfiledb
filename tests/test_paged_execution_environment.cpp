#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "strata/application.h"
#include "strata/domain/ast.h"
#include "strata/domain/import.h"
#include "strata/domain/plan.h"
#include "strata/domain/value.h"
#include "strata/paged/paged_engine.h"
#include "strata/sql_parser.h"
#include "test_equivalence_support.h"
#include "test_harness.h"
#include "test_support.h"

#ifndef STRATA_SOURCE_DIR
#error "STRATA_SOURCE_DIR must be defined by the build"
#endif

// The real data volume of data/ and the sample database of sample_my_db/ on
// both engines (docs/PagedExecutionTestDocument.md). Every file the tests write
// is in a temporary directory; data/ and sample_my_db/ are only read.

namespace strata::testing {

namespace {

using application::QueryService;
using domain::ExecutionResult;
using paged::ReplacementPolicyKind;

constexpr std::size_t kPagedFrames = 64;  // 256 KiB, far below the data volume

std::filesystem::path SourceDirectory() {
  return std::filesystem::path(STRATA_SOURCE_DIR);
}

std::filesystem::path DataDirectory() { return SourceDirectory() / "data"; }

bool HasData() {
  std::error_code error;
  return std::filesystem::exists(DataDirectory() / "users.csv", error) &&
         std::filesystem::exists(DataDirectory() / "posts.csv", error) &&
         std::filesystem::exists(DataDirectory() / "engagements.csv", error);
}

// Copies the regular files of `from` into `to` (created when missing). Returns
// false and reports a failure when a copy fails.
bool CopyFiles(const std::filesystem::path& from,
               const std::filesystem::path& to) {
  std::error_code error;
  std::filesystem::create_directories(to, error);
  for (const auto& entry : std::filesystem::directory_iterator(from, error)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    // The lock file of a database directory is not data.
    if (entry.path().filename() == ".strata.lock") {
      continue;
    }
    std::filesystem::copy_file(
        entry.path(), to / entry.path().filename(),
        std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
      ReportFailure(
          __FILE__, __LINE__,
          "cannot copy " + entry.path().string() + ": " + error.message());
      return false;
    }
  }
  return !error;
}

// ----------------------------------------------------------------------------
// Two engines over a copy of data/.
// ----------------------------------------------------------------------------

// The imports of the README: users.csv with the id typed as INT and the three
// malformed rows skipped, the other two files as they are.
std::vector<domain::ImportRequest> DataImports(
    const std::filesystem::path& source) {
  domain::ImportRequest users;
  users.file_path = (source / "users.csv").string();
  users.primary_key = "id";
  users.column_types.emplace_back("id", domain::DataType::kInt);
  users.should_skip_bad_rows = true;
  domain::ImportRequest posts;
  posts.file_path = (source / "posts.csv").string();
  domain::ImportRequest engagements;
  engagements.file_path = (source / "engagements.csv").string();
  return {users, posts, engagements};
}

// A flat-file engine and a paged engine, each with its own directory under one
// temporary directory, both able to run `.import`.
struct DataEngines {
  std::unique_ptr<QueryService> flatfile;
  PagedFixture paged;
};

// Creates the two empty databases under `root`.
bool OpenDataEngines(const std::filesystem::path& root, DataEngines* engines) {
  std::error_code error;
  std::filesystem::create_directories(root / "flatfile", error);
  std::filesystem::create_directories(root / "paged", error);
  engines->flatfile = MakeLoadedService(root / "flatfile");
  engines->paged = MakeFilePagedFixture(root / "paged", kPagedFrames,
                                        ReplacementPolicyKind::kLru);
  return engines->flatfile != nullptr && engines->paged.service != nullptr;
}

// The queries of the README that read data/, then twelve more join, aggregate,
// sort and LIMIT queries. Every LIMIT sits behind an ORDER BY that ends with a
// unique key, so the cut is the same on both engines.
const std::vector<std::string>& DataQueries() {
  static const std::vector<std::string> queries = {
      "SELECT username, location FROM users WHERE id = 1",
      "SELECT id, UPPER(username), LENGTH(username) FROM users WHERE username "
      "LIKE 'kn%' ORDER BY id LIMIT 2",
      "SELECT COUNT(*), MIN(timestamp), MAX(timestamp), AVG(timestamp) FROM "
      "engagements",
      "SELECT e.type, COUNT(*) FROM engagements e JOIN users u ON e.username = "
      "u.username WHERE u.location = 'Patriciashire' GROUP BY e.type",
      "SELECT location, COUNT(*) FROM users GROUP BY location ORDER BY 2 DESC, "
      "1 LIMIT 10",
      "SELECT p.id, u.location FROM posts p JOIN users u ON p.username = "
      "u.username ORDER BY p.id LIMIT 50",
      "SELECT u.location, COUNT(*), SUM(p.views) FROM users u JOIN posts p ON "
      "p.username = u.username JOIN engagements e ON e.postid = p.id GROUP BY "
      "u.location ORDER BY 2 DESC, 1 LIMIT 10",
      "SELECT type, COUNT(*), MIN(timestamp), MAX(timestamp) FROM engagements "
      "GROUP BY type ORDER BY type",
      "SELECT username, COUNT(*) FROM posts GROUP BY username ORDER BY 2 DESC, "
      "1 LIMIT 10",
      "SELECT id, content, views FROM posts ORDER BY views DESC, id LIMIT 10",
      "SELECT COUNT(*) FROM users u JOIN posts p ON p.username = u.username",
      "SELECT e.id, p.id, u.id FROM engagements e JOIN posts p ON e.postid = "
      "p.id JOIN users u ON e.username = u.username WHERE e.type = 'comment' "
      "ORDER BY e.id LIMIT 10",
      "SELECT LENGTH(content), COUNT(*) FROM posts GROUP BY LENGTH(content) "
      "ORDER BY 1 LIMIT 15",
      "SELECT SUM(views), MIN(views), MAX(views), AVG(views) FROM posts",
      "SELECT p.username, MAX(e.timestamp) FROM posts p JOIN engagements e ON "
      "e.postid = p.id GROUP BY p.username ORDER BY 2 DESC, 1 LIMIT 5",
      "SELECT e.username, COUNT(*) FROM engagements e GROUP BY e.username "
      "ORDER BY 2 DESC, 1 LIMIT 10",
      "SELECT username, id FROM users ORDER BY username, id LIMIT 10 OFFSET "
      "5000",
      "SELECT id, username FROM users WHERE id < 0",
      "SELECT AVG(views) FROM posts WHERE id < 0",
  };
  return queries;
}

}  // namespace

TEST(test_environment_data_directory_queries_match_on_both_engines) {
  if (!HasData()) {
    SKIP("data/ is missing");
  }
  TempDir work;
  if (!CopyFiles(DataDirectory(), work.path() / "source")) {
    return;
  }
  DataEngines engines;
  if (!OpenDataEngines(work.path(), &engines)) {
    return;
  }
  EquivalenceRunner runner(*engines.flatfile, *engines.paged.service,
                           "data volume");

  for (const domain::ImportRequest& request :
       DataImports(work.path() / "source")) {
    if (!ReportDifference(__FILE__, __LINE__, runner.RunImport(request))) {
      return;
    }
    const domain::CommandComplete* complete =
        AsCommandComplete(runner.last_paged_result());
    CHECK(complete != nullptr);
  }
  for (const std::string& sql : DataQueries()) {
    if (!RUN_EQUIVALENT(runner, sql)) {
      return;
    }
  }

  // The data is far larger than the buffer pool, and no query left a pin.
  const paged::BufferStats stats =
      engines.paged.engine->database().buffer().Stats();
  CHECK(stats.evictions > 0);
  CHECK_EQ(engines.paged.engine->database().buffer().PinnedFrameCount(),
           std::size_t{0});
  CHECK(engines.paged.engine->database().buffer().PageCount() > 540);
}

// ----------------------------------------------------------------------------
// The sample database in sample_my_db/.
// ----------------------------------------------------------------------------

namespace {

// Copies every table of `flatfile` (schema with its constraints, then the rows)
// into the paged engine, parents before children. Reports a failure and returns
// false when a step does not succeed.
bool CopyCatalogAndRows(QueryService* flatfile, paged::PagedEngine* engine) {
  for (const domain::TableSchema& schema : flatfile->DescribeCatalog()) {
    domain::PlanNode create;
    create.node = domain::CreateTableNode{schema};
    ExecutionResult created = engine->Execute(create);
    if (AsCommandComplete(created) == nullptr) {
      ReportFailure(
          __FILE__, __LINE__,
          "cannot create " + schema.name + ": " + DescribeResult(created));
      return false;
    }
    ExecutionResult selected =
        ExecuteSql(*flatfile, "SELECT * FROM " + schema.name);
    const domain::ResultSet* rows = AsResultSet(selected);
    if (rows == nullptr) {
      ReportFailure(__FILE__, __LINE__, "cannot read " + schema.name);
      return false;
    }
    domain::PlanNode insert;
    insert.node = domain::InsertNode{schema.name, rows->rows, std::nullopt};
    ExecutionResult inserted = engine->Execute(insert);
    if (AsAffectedRows(inserted) == nullptr) {
      ReportFailure(
          __FILE__, __LINE__,
          "cannot fill " + schema.name + ": " + DescribeResult(inserted));
      return false;
    }
  }
  return true;
}

// The queries of the README and more, then writes that hit every constraint,
// a four-table join, and a cascade through a new table that references the
// UNIQUE column courses.code.
const std::vector<std::string>& SampleStatements() {
  static const std::vector<std::string> statements = {
      "SELECT code, title, credits, capacity FROM courses ORDER BY code LIMIT "
      "4",
      "SELECT code, COUNT(*) FROM courses JOIN enrollments ON course_id = "
      "courses.id WHERE semester = '2026-Spring' AND status = 'enrolled' GROUP "
      "BY code ORDER BY 2 DESC, 1 LIMIT 3",
      "SELECT name, code, score FROM students JOIN enrollments ON student_id = "
      "students.id JOIN courses ON course_id = courses.id WHERE semester = "
      "'2025-Fall' AND status = 'completed' ORDER BY 3 DESC, 1 LIMIT 3",
      "SELECT id, name, LENGTH(name), UPPER(major) FROM students WHERE name "
      "LIKE '\xE7\x8E\x8B%'",
      // Four tables.
      "SELECT i.name, c.code, s.name, e.score, e.id FROM instructors i JOIN "
      "courses c ON c.instructor_id = i.id JOIN enrollments e ON e.course_id = "
      "c.id "
      "JOIN students s ON e.student_id = s.id WHERE e.status = 'completed' "
      "ORDER BY e.score DESC, e.id LIMIT 10",
      "SELECT i.dept, COUNT(*), AVG(e.score) FROM instructors i JOIN courses c "
      "ON c.instructor_id = i.id JOIN enrollments e ON e.course_id = c.id JOIN "
      "students s ON e.student_id = s.id GROUP BY i.dept ORDER BY 1",
      "SELECT major, COUNT(*), MIN(year), MAX(year) FROM students GROUP BY "
      "major ORDER BY 2 DESC, 1",
      "SELECT status, COUNT(*), SUM(score) FROM enrollments GROUP BY status "
      "ORDER BY 1",
      "SELECT c.code, COUNT(*) AS n FROM courses c JOIN enrollments e ON "
      "e.course_id = c.id GROUP BY c.code ORDER BY n DESC, c.code",
      "SELECT s.name, c.title, s.id, e.id FROM students s JOIN enrollments e "
      "ON "
      "e.student_id = s.id JOIN courses c ON e.course_id = c.id WHERE "
      "c.capacity < 6 ORDER BY s.id, e.id LIMIT 8 OFFSET 2",
      "SELECT * FROM students WHERE year > 99",
      "SELECT AVG(score) FROM enrollments WHERE semester = 'none'",
      // Inserts: defaults, auto-increment, and every kind of rejection.
      "INSERT INTO enrollments (student_id, course_id, semester) VALUES (1001, "
      "1, '2026-Spring')",
      "SELECT id, status, score FROM enrollments WHERE semester = "
      "'2026-Spring' "
      "AND student_id = 1001 AND course_id = 1",
      "INSERT INTO enrollments (student_id, course_id, semester) VALUES (9999, "
      "1, '2026-Spring')",
      "INSERT INTO enrollments (student_id, course_id, semester) VALUES (1001, "
      "999, '2026-Spring')",
      "INSERT INTO students VALUES (1001, 'Dup', 'ART', 1, 'dup@campus.edu')",
      "INSERT INTO students VALUES (2000, 'New', 'ART', 1, "
      "'s1001@campus.edu')",
      "INSERT INTO courses (code, title, credits, dept, capacity, "
      "instructor_id) VALUES ('NEW101', 'New One', 3, 'CS', 10, 1), ('NEW102', "
      "'New Two', 3, 'CS', 10, 2)",
      "INSERT INTO courses (code, title, credits, dept, capacity, "
      "instructor_id) VALUES ('NEW103', 'New Three', 3, 'CS', 10, 99)",
      // Updates and deletes.
      "UPDATE enrollments SET status = 'completed', score = 90 WHERE id = 100",
      "DELETE FROM students WHERE id = 1001",
      "DELETE FROM instructors WHERE id = 1",
      "UPDATE courses SET capacity = capacity + 1 WHERE dept = 'CS'",
      "UPDATE courses SET instructor_id = 99 WHERE id = 1",
      "UPDATE students SET email = 's1002@campus.edu' WHERE id = 1001",
      "UPDATE students SET name = UPPER(name) WHERE major = 'ART'",
      "DELETE FROM enrollments WHERE semester = '2025-Fall' AND status = "
      "'completed' AND score < 70",
      // A cascade: the new table references the UNIQUE column courses.code.
      "CREATE TABLE course_notes (id INT PRIMARY KEY AUTOINCREMENT, "
      "course_code TEXT REFERENCES courses(code), note TEXT)",
      "INSERT INTO course_notes (course_code, note) VALUES ('CS101', 'a'), "
      "('CS101', 'b'), ('CS201', 'c'), ('NEW101', 'd')",
      "INSERT INTO course_notes (course_code, note) VALUES ('NOPE', 'e')",
      "UPDATE courses SET code = 'CS101X' WHERE id = 1",
      "SELECT course_code, COUNT(*) FROM course_notes GROUP BY course_code "
      "ORDER BY 1",
      "UPDATE courses SET code = 'CS201' WHERE code = 'CS101X'",
      "DELETE FROM courses WHERE code = 'NEW101'",
      "DELETE FROM course_notes WHERE course_code = 'NEW101'",
      "DELETE FROM courses WHERE code = 'NEW101'",
      "SELECT id, code, instructor_id FROM courses ORDER BY id",
  };
  return statements;
}

}  // namespace

TEST(test_environment_sample_database_matches_on_both_engines) {
  const std::filesystem::path sample = SourceDirectory() / "sample_my_db";
  std::error_code error;
  if (!std::filesystem::exists(sample / "catalog.csv", error)) {
    SKIP("sample_my_db/ is missing");
  }
  TempDir work;
  if (!CopyFiles(sample, work.path() / "flatfile")) {
    return;
  }
  std::error_code create_error;
  std::filesystem::create_directories(work.path() / "paged", create_error);
  std::unique_ptr<QueryService> flatfile =
      MakeLoadedService(work.path() / "flatfile");
  PagedFixture paged = MakeFilePagedFixture(work.path() / "paged", 16,
                                            ReplacementPolicyKind::kLru);
  if (flatfile == nullptr || paged.service == nullptr) {
    return;
  }
  if (!CopyCatalogAndRows(flatfile.get(), paged.engine)) {
    return;
  }

  EquivalenceRunner runner(*flatfile, *paged.service, "sample database");
  // Both engines hold the same tables before the first statement.
  CHECK_EQ(runner.flatfile_tables().size(), std::size_t{4});
  CHECK(runner.flatfile_tables() == runner.paged_tables());
  for (const std::string& sql : SampleStatements()) {
    if (!RUN_EQUIVALENT(runner, sql)) {
      return;
    }
  }
  for (const domain::TableSchema& schema : flatfile->DescribeCatalog()) {
    if (!RUN_EQUIVALENT(runner, "SELECT * FROM " + schema.name)) {
      return;
    }
  }
  CHECK_EQ(paged.engine->database().buffer().PinnedFrameCount(),
           std::size_t{0});
}

// ----------------------------------------------------------------------------
// Timings.
// ----------------------------------------------------------------------------

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kImportRepetitions = 3;
constexpr std::size_t kQueryRepetitions = 5;

double MillisecondsSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}

double Median(std::vector<double> samples) {
  std::ranges::sort(samples);
  return samples[samples.size() / 2];
}

// One timed workload: the statements run in order, together they are one
// sample.
struct TimedStatement {
  std::string label;
  std::vector<std::string> sql;
};

// Runs the statements of the workload once on the service; reports a failure
// when one is not a result set.
bool RunWorkload(QueryService* service, const std::vector<std::string>& sql) {
  for (const std::string& text : sql) {
    adapter::ParseResult parsed = adapter::ParseStatement(text);
    const auto* statement = std::get_if<domain::Statement>(&parsed);
    if (statement == nullptr) {
      ReportFailure(__FILE__, __LINE__, "does not parse: " + text);
      return false;
    }
    ExecutionResult result = service->Execute(*statement);
    if (AsResultSet(result) == nullptr) {
      ReportFailure(__FILE__, __LINE__,
                    text + " returned " + DescribeResult(result));
      return false;
    }
  }
  return true;
}

// The median of kQueryRepetitions runs of the workload on one service, in
// milliseconds, or a negative number after a failure.
double TimeWorkload(QueryService* service,
                    const std::vector<std::string>& sql) {
  std::vector<double> samples;
  for (std::size_t i = 0; i < kQueryRepetitions; ++i) {
    const Clock::time_point start = Clock::now();
    if (!RunWorkload(service, sql)) {
      return -1.0;
    }
    samples.push_back(MillisecondsSince(start));
  }
  return Median(samples);
}

// The three imports into fresh databases, kImportRepetitions times: the median
// of the whole load, in milliseconds, or a negative number after a failure.
double TimeImports(const std::filesystem::path& source, bool is_paged) {
  std::vector<double> samples;
  for (std::size_t i = 0; i < kImportRepetitions; ++i) {
    TempDir root;
    DataEngines engines;
    if (!OpenDataEngines(root.path(), &engines)) {
      return -1.0;
    }
    QueryService* service =
        is_paged ? engines.paged.service.get() : engines.flatfile.get();
    const Clock::time_point start = Clock::now();
    for (const domain::ImportRequest& request : DataImports(source)) {
      ExecutionResult result = service->ImportCsv(request);
      if (AsCommandComplete(result) == nullptr) {
        ReportFailure(__FILE__, __LINE__,
                      "import failed: " + DescribeResult(result));
        return -1.0;
      }
    }
    samples.push_back(MillisecondsSince(start));
  }
  return Median(samples);
}

void PrintTiming(const std::string& workload, double flatfile_ms,
                 double paged_ms) {
  std::cout << "[timing] " << workload << ": flatfile " << flatfile_ms
            << " ms, paged " << paged_ms << " ms (median)\n"
            << std::flush;
}

}  // namespace

TEST(test_environment_timing_of_both_engines) {
  if (!HasData()) {
    SKIP("data/ is missing");
  }
  TempDir work;
  if (!CopyFiles(DataDirectory(), work.path() / "source")) {
    return;
  }
  const std::filesystem::path source = work.path() / "source";

  // The load: fresh databases each time.
  const double flatfile_import = TimeImports(source, false);
  const double paged_import = TimeImports(source, true);
  PrintTiming("import of users, posts and engagements (" +
                  std::to_string(kImportRepetitions) + " runs)",
              flatfile_import, paged_import);

  // The queries run on one loaded pair, which is also compared for equivalence.
  DataEngines engines;
  if (!OpenDataEngines(work.path() / "queries", &engines)) {
    return;
  }
  EquivalenceRunner runner(*engines.flatfile, *engines.paged.service,
                           "timings");
  for (const domain::ImportRequest& request : DataImports(source)) {
    if (!ReportDifference(__FILE__, __LINE__, runner.RunImport(request))) {
      return;
    }
  }
  const std::vector<std::string> readme_queries = {
      DataQueries()[0], DataQueries()[1], DataQueries()[2], DataQueries()[3]};
  const std::vector<TimedStatement> workloads = {
      {"the four README queries", readme_queries},
      {"three-table join with GROUP BY",
       {"SELECT u.location, COUNT(*), SUM(p.views) FROM users u JOIN posts p "
        "ON p.username = u.username JOIN engagements e ON e.postid = p.id "
        "GROUP BY u.location ORDER BY 2 DESC, 1 LIMIT 10"}},
      {"LIMIT 10 behind ORDER BY id",
       {"SELECT id, username FROM users ORDER BY id LIMIT 10"}},
  };
  for (const TimedStatement& workload : workloads) {
    for (const std::string& sql : workload.sql) {
      if (!RUN_EQUIVALENT(runner, sql)) {
        return;
      }
    }
    const double flatfile_ms =
        TimeWorkload(engines.flatfile.get(), workload.sql);
    const double paged_ms =
        TimeWorkload(engines.paged.service.get(), workload.sql);
    PrintTiming(
        workload.label + " (" + std::to_string(kQueryRepetitions) + " runs)",
        flatfile_ms, paged_ms);
  }

  // LIMIT 10 without ORDER BY stops early on paged. Which ten rows come back
  // is undefined (2.6), so only the row count and the columns are compared.
  const std::string unordered = "SELECT id, username FROM users LIMIT 10";
  const std::vector<std::string> unordered_sql = {unordered};
  for (QueryService* service :
       {engines.flatfile.get(), engines.paged.service.get()}) {
    const domain::ResultSet* rows = nullptr;
    ExecutionResult result = ExecuteSql(*service, unordered);
    rows = AsResultSet(result);
    CHECK(rows != nullptr && rows->rows.size() == 10 &&
          rows->columns.size() == 2);
  }
  PrintTiming("LIMIT 10 without ORDER BY (" +
                  std::to_string(kQueryRepetitions) + " runs)",
              TimeWorkload(engines.flatfile.get(), unordered_sql),
              TimeWorkload(engines.paged.service.get(), unordered_sql));
}

}  // namespace strata::testing
