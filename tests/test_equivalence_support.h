#ifndef STRATA_TESTS_TEST_EQUIVALENCE_SUPPORT_H_
#define STRATA_TESTS_TEST_EQUIVALENCE_SUPPORT_H_

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "strata/application.h"
#include "strata/domain/import.h"
#include "strata/domain/value.h"
#include "strata/paged/paged_engine.h"
#include "strata/paged/paged_types.h"

// The cross-engine comparison of docs/DesignDocument.md section
// 2.8 (ADR-0044), shared by the integration tests and the environment tests.
// Two QueryServices, one over a flat-file engine and one over a paged engine,
// run the same statement; the runner then compares the two results and the
// content of every table. Every function returns the description of the first
// difference (or an empty optional), so a test decides how to report it.

namespace strata::testing {

// ----------------------------------------------------------------------------
// Result comparison (pure).
// ----------------------------------------------------------------------------

// The description of the first difference between the results the two engines
// returned for one statement, or no value when they are equivalent under 2.8:
// the same result kind; for a ResultSet the same columns and either the same
// multiset of rows (sort_columns empty) or the same sequence of sort keys with
// the rows of equal keys compared as a multiset; the same AffectedRows count,
// CommandComplete tag and notes; the same ExecutionError kind and message.
// sort_columns are positions in the result columns.
std::optional<std::string> FindResultDifference(
    const domain::ExecutionResult& flatfile,
    const domain::ExecutionResult& paged,
    const std::vector<std::size_t>& sort_columns);

// The first difference between two catalogs (table names, columns with their
// default values, primary key, foreign keys, UNIQUE columns, auto-increment),
// in catalog order.
std::optional<std::string> FindCatalogDifference(
    const std::vector<domain::TableSchema>& flatfile,
    const std::vector<domain::TableSchema>& paged);

// One line per result: the kind and, for a result set, the columns and the
// first rows, for the failure messages.
std::string DescribeResult(const domain::ExecutionResult& result);

// Rows in a fixed total order, so that two multisets of rows compare with ==.
std::vector<domain::Row> SortedRows(std::vector<domain::Row> rows);

// ----------------------------------------------------------------------------
// The two services.
// ----------------------------------------------------------------------------

// A QueryService over a paged engine, plus the engine it holds.
struct PagedFixture {
  std::unique_ptr<application::QueryService> service;
  // Borrowed from `service`; valid until `service` is destroyed. Calling it
  // directly bypasses the database-level lock, which is fine on one thread.
  paged::PagedEngine* engine = nullptr;
};

// A paged engine over a MemoryPageStore. Reports a failure and returns an
// empty fixture when the database cannot be opened.
PagedFixture MakeMemoryPagedFixture(std::size_t frames,
                                    paged::ReplacementPolicyKind policy);

// A paged engine over directory/paged.db (the directory must exist).
PagedFixture MakeFilePagedFixture(const std::filesystem::path& directory,
                                  std::size_t frames,
                                  paged::ReplacementPolicyKind policy);

// ----------------------------------------------------------------------------
// The runner.
// ----------------------------------------------------------------------------

// A table's rows, sorted with SortedRows, by lowercase table name.
using TableContents = std::map<std::string, std::vector<domain::Row>>;

// Runs the same statements on a flat-file service and a paged service and
// compares after each one. Borrows both services for its whole life; they must
// not be used by anyone else, because the runner remembers the table contents
// of the last statement. Single-threaded.
class EquivalenceRunner {
 public:
  // label prefixes every difference (a seed or a corpus name). The constructor
  // snapshots both services; a snapshot that fails is reported by the first
  // Run. The two snapshots are not compared with each other: a difference in
  // the starting data shows at the first write or the first SELECT.
  EquivalenceRunner(application::QueryService& flatfile,
                    application::QueryService& paged, std::string label);

  // Parses sql, executes it on both services, and compares the results (2.8).
  // After any statement other than a SELECT it also compares the two catalogs
  // and the content of every table, and checks that a rejected statement
  // changed no table of either engine. A SELECT with LIMIT or OFFSET must have
  // a deterministic cut: the statement has an ORDER BY, and no sort key is
  // shared by the row before a cut and the row after it; otherwise Run reports
  // the statement itself as outside the comparable set. Returns the first
  // difference, with the label, the sequence number and the statement text.
  std::optional<std::string> Run(const std::string& sql);

  // As Run, for `.import` (QueryService::ImportCsv).
  std::optional<std::string> RunImport(const domain::ImportRequest& request);

  // The most recent result of each service (after Run or RunImport).
  const domain::ExecutionResult& last_flatfile_result() const {
    return last_flatfile_result_;
  }
  const domain::ExecutionResult& last_paged_result() const {
    return last_paged_result_;
  }
  // The table contents after the last write statement, per engine.
  const TableContents& flatfile_tables() const { return flatfile_tables_; }
  const TableContents& paged_tables() const { return paged_tables_; }

  // The number of statements run so far.
  std::size_t statement_count() const { return statement_count_; }

 private:
  // The result comparison and, when `is_write`, the state comparison.
  std::optional<std::string> Compare(const std::string& statement_text,
                                     bool is_write,
                                     const std::vector<std::size_t>& sort_keys);
  std::optional<std::string> CompareState(bool is_rejected);
  std::string Describe(const std::string& statement_text,
                       const std::string& difference) const;

  application::QueryService& flatfile_;
  application::QueryService& paged_;
  std::string label_;
  std::size_t statement_count_ = 0;
  domain::ExecutionResult last_flatfile_result_ = domain::AffectedRows{0};
  domain::ExecutionResult last_paged_result_ = domain::AffectedRows{0};
  TableContents flatfile_tables_;
  TableContents paged_tables_;
  std::optional<std::string> snapshot_failure_;
};

// Reports `difference` as a failure of the running test at file:line and
// returns false; returns true when there is none.
bool ReportDifference(const char* file, int line,
                      const std::optional<std::string>& difference);

}  // namespace strata::testing

// Runs sql on both engines of `runner` and fails the running test on the first
// difference. Evaluates to true when the two are equivalent.
#define RUN_EQUIVALENT(runner, sql) \
  ::strata::testing::ReportDifference(__FILE__, __LINE__, (runner).Run(sql))

#endif  // STRATA_TESTS_TEST_EQUIVALENCE_SUPPORT_H_
