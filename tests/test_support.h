#ifndef FLATFILEDB_TESTS_TEST_SUPPORT_H_
#define FLATFILEDB_TESTS_TEST_SUPPORT_H_

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "flatfiledb/application.h"
#include "flatfiledb/domain/engine.h"
#include "flatfiledb/domain/plan.h"
#include "flatfiledb/domain/value.h"
#include "flatfiledb/flat_file_engine.h"
#include "flatfiledb/infrastructure.h"
#include "test_harness.h"

// Shared helpers for the test files: temporary directories, the CSV fixtures
// of docs/TestDocument.md section 1, a fake catalog, a decorator repository,
// and small accessors for ExecutionResult.

namespace flatfiledb::testing {

// A unique directory under the system temp directory, removed on destruction.
// The destructor restores write permission first, so tests may chmod it.
class TempDir {
 public:
  TempDir();
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void WriteFile(const std::filesystem::path& path, const std::string& content);
std::string ReadFile(const std::filesystem::path& path);

// Writes catalog.csv for the three tables of the `data/` example (users,
// posts, engagements; users.username is UNIQUE).
void WriteCatalogFixture(const std::filesystem::path& dir);

// Each of the following writes catalog.csv and the table files of the fixture
// with "\n" line endings.
void WriteFixtureA(const std::filesystem::path& dir);
void WriteFixtureB(const std::filesystem::path& dir);
void WriteFixtureC1(const std::filesystem::path& dir);
void WriteFixtureC2(const std::filesystem::path& dir);
// Fixture D: a catalog with notes(id INT PRIMARY KEY, body TEXT) and a
// notes.csv with quoted fields.
void WriteFixtureD(const std::filesystem::path& dir);

// A CsvRepository over the database directory dir.
std::unique_ptr<infrastructure::CsvRepository> MakeCsvRepository(
    const std::filesystem::path& dir);

// The schemas of the `data/` example in docs/DesignDocument.md section 2.2
// (users.username is UNIQUE), plus the test-only table measures(id INT, score
// DOUBLE).
domain::TableSchema UsersSchema();
domain::TableSchema PostsSchema();
domain::TableSchema EngagementsSchema();
domain::TableSchema MeasuresSchema();
// Test-only tables for INSERT that omits columns: drafts(id INT PRIMARY KEY,
// body TEXT, owner TEXT DEFAULT 'none', score DOUBLE DEFAULT 1.0) and
// jobs(id INT PRIMARY KEY AUTOINCREMENT, body TEXT).
domain::TableSchema DraftsSchema();
domain::TableSchema JobsSchema();

// ICatalog over the schemas above (users, posts, engagements, measures,
// drafts, jobs).
class FakeCatalog : public domain::ICatalog {
 public:
  std::optional<domain::TableSchema> FindTable(
      const std::string& name) const override;
  std::vector<std::string> TableNames() const override;
};

// What the decorator saw, shared with the test that created it.
struct RepositoryLog {
  int write_calls = 0;  // Commit calls
  std::vector<std::vector<std::string>>
      written_table_names;  // the table writes of each Commit call
  int append_calls = 0;
  std::optional<std::string> write_failure;   // when set, Commit returns it
  std::optional<std::string> append_failure;  // when set, AppendRow returns it
};

// Wraps a real repository, records calls in log, and can be told to fail.
class DecoratorRepository : public infrastructure::IFlatFileRepository {
 public:
  DecoratorRepository(
      std::unique_ptr<infrastructure::IFlatFileRepository> inner,
      std::shared_ptr<RepositoryLog> log);

  infrastructure::LoadOutcome Load(bool is_parallel) override;
  infrastructure::InspectOutcome InspectTable(
      const domain::TableSchema& schema) override;
  std::optional<std::string> Commit(
      const infrastructure::CommitSet& set) override;
  std::optional<std::string> AppendRow(const domain::TableSchema& schema,
                                       const domain::Row& row) override;

 private:
  std::unique_ptr<infrastructure::IFlatFileRepository> inner_;
  std::shared_ptr<RepositoryLog> log_;
};

// Parses sql; reports a test failure and returns an empty SelectStatement when
// the text does not parse.
domain::Statement MustParse(const std::string& sql);

// Parses and binds sql against catalog; reports a test failure and returns
// null when either step fails.
domain::PlanPtr MustBind(const std::string& sql,
                         const domain::ICatalog& catalog);

// Parses, binds and executes sql through a QueryService. A parse error is
// returned as ExecutionError{kBind, "parse error: ..."} so tests can assert on
// one result type.
domain::ExecutionResult ExecuteSql(application::QueryService& service,
                                   const std::string& sql);

// A QueryService over a FlatFileEngine loaded from the database directory dir.
// Reports a test failure and returns null when loading fails.
std::unique_ptr<application::QueryService> MakeLoadedService(
    const std::filesystem::path& dir,
    std::shared_ptr<RepositoryLog> log = nullptr);

const domain::ResultSet* AsResultSet(const domain::ExecutionResult& result);
const domain::AffectedRows* AsAffectedRows(
    const domain::ExecutionResult& result);
const domain::CommandComplete* AsCommandComplete(
    const domain::ExecutionResult& result);
const domain::ExecutionError* AsError(const domain::ExecutionResult& result);

// Each row as its FormatValue cells joined by a Tab.
std::vector<std::string> RowsAsText(const domain::ResultSet& result_set);

}  // namespace flatfiledb::testing

#endif  // FLATFILEDB_TESTS_TEST_SUPPORT_H_
