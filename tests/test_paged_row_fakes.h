#ifndef STRATA_TESTS_TEST_PAGED_ROW_FAKES_H_
#define STRATA_TESTS_TEST_PAGED_ROW_FAKES_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "strata/domain/engine.h"
#include "strata/domain/value.h"
#include "strata/paged/paged_types.h"
#include "strata/paged/row_stream.h"

// In-memory fakes of the row layer's interfaces, shared by the operator tests
// (docs/TestDocument.md) and the mutation planner tests. Nothing
// here touches a page.
//
// How to use them:
//   * FakeTableReader is an ITableReader over tables held as vectors. Register
//     a table with AddTable(name, columns, rows) or AddTable(schema, rows);
//     table names are case-insensitive, and an unknown table yields kBind
//     "unknown table: <name>" with the name as given. Every Scan hands out a
//     new VectorRowStream over its own copy of the rows, so several scans of
//     one table are independent.
//   * Each table has a StreamLog, shared with every stream scanned from it.
//     reader.LogOf("t")->scan_calls counts Scan calls, ->next_calls counts
//     Next calls (also those after the end of the rows or after an error), so
//     a test can read the counts after the streams have moved into an operator
//     tree. Setting ->fail_on_call = n makes the n-th Next call counted in
//     next_calls (1-based, over all streams of the table, not reset by a new
//     Scan) return ->failure; the stream that fails is over afterwards and
//     returns the end of the rows, as IRowStream requires.
//   * Rows get the Rid FakeRid(position), where position is the index of the
//     row in the vector given to AddTable, so a test can predict the Rid of a
//     row. AddStoredTable takes StoredRow values when the test needs its own
//     Rids.
//   * MapCatalog is an ICatalog over registered schemas, in registration
//     order, for tests that bind SQL or call the mutation planner.
//   * Not thread-safe, apart from what ITableReader::Scan promises: do not
//     share a FakeTableReader between threads that run at the same time.

namespace strata::testing {

// What the streams of one table saw. Shared (std::shared_ptr) between the
// table in a FakeTableReader and every stream it hands out.
struct StreamLog {
  std::size_t scan_calls = 0;  // FakeTableReader::Scan calls for this table
  std::size_t next_calls = 0;  // IRowStream::Next calls, whatever they return
  // When set, the Next call whose number in next_calls equals this value
  // (counting from 1) returns failure instead of a row.
  std::optional<std::size_t> fail_on_call = std::nullopt;
  domain::ExecutionError failure{domain::ErrorKind::kIo,
                                 "injected stream failure"};
};

// Rows per fake page in the Rids that FakeRid makes up.
inline constexpr std::size_t kFakeSlotsPerPage = 100;

// The Rid given to the row at `position` (counting from 0) of a fake table:
// page 1 + position / 100, slot position % 100.
inline paged::Rid FakeRid(std::size_t position) {
  return paged::Rid{
      static_cast<paged::PageId>(1 + position / kFakeSlotsPerPage),
      static_cast<std::uint16_t>(position % kFakeSlotsPerPage)};
}

// The rows with Rids FakeRid(0), FakeRid(1), ...
inline std::vector<paged::StoredRow> WithFakeRids(
    const std::vector<domain::Row>& rows) {
  std::vector<paged::StoredRow> stored;
  stored.reserve(rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    stored.push_back(paged::StoredRow{FakeRid(i), rows[i]});
  }
  return stored;
}

// An IRowStream over a vector. Moves each row out as it returns it.
class VectorRowStream : public paged::IRowStream {
 public:
  // log may be null when the test does not look at the counts.
  VectorRowStream(std::vector<domain::ColumnDef> columns,
                  std::vector<paged::StoredRow> rows,
                  std::shared_ptr<StreamLog> log = nullptr)
      : columns_(std::move(columns)),
        rows_(std::move(rows)),
        log_(log != nullptr ? std::move(log) : std::make_shared<StreamLog>()) {}

  paged::StreamResult Next() override {
    ++log_->next_calls;
    if (is_over_) {
      return std::optional<paged::StoredRow>();
    }
    if (log_->fail_on_call.has_value() &&
        *log_->fail_on_call == log_->next_calls) {
      is_over_ = true;
      return log_->failure;
    }
    if (position_ >= rows_.size()) {
      is_over_ = true;
      return std::optional<paged::StoredRow>();
    }
    return std::optional<paged::StoredRow>(std::move(rows_[position_++]));
  }

  std::vector<domain::ColumnDef> Columns() const override { return columns_; }

 private:
  std::vector<domain::ColumnDef> columns_;
  std::vector<paged::StoredRow> rows_;
  std::shared_ptr<StreamLog> log_;
  std::size_t position_ = 0;
  bool is_over_ = false;
};

// An ITableReader over tables held in memory.
class FakeTableReader : public paged::ITableReader {
 public:
  // Registers a table, replacing one with the same name (and its log).
  void AddStoredTable(const std::string& name,
                      std::vector<domain::ColumnDef> columns,
                      std::vector<paged::StoredRow> rows) {
    tables_[domain::ToLower(name)] = Table{std::move(columns), std::move(rows),
                                           std::make_shared<StreamLog>()};
  }

  void AddTable(const std::string& name, std::vector<domain::ColumnDef> columns,
                const std::vector<domain::Row>& rows) {
    AddStoredTable(name, std::move(columns), WithFakeRids(rows));
  }

  void AddTable(const domain::TableSchema& schema,
                const std::vector<domain::Row>& rows) {
    AddStoredTable(schema.name, schema.columns, WithFakeRids(rows));
  }

  // The log of the table, or null when the table is not registered.
  std::shared_ptr<StreamLog> LogOf(const std::string& table) const {
    auto found = tables_.find(domain::ToLower(table));
    return found == tables_.end() ? nullptr : found->second.log;
  }

  paged::ScanResult Scan(const std::string& table) const override {
    auto found = tables_.find(domain::ToLower(table));
    if (found == tables_.end()) {
      return domain::ExecutionError{domain::ErrorKind::kBind,
                                    "unknown table: " + table};
    }
    const Table& entry = found->second;
    ++entry.log->scan_calls;
    return std::unique_ptr<paged::IRowStream>(std::make_unique<VectorRowStream>(
        entry.columns, entry.rows, entry.log));
  }

 private:
  struct Table {
    std::vector<domain::ColumnDef> columns;
    std::vector<paged::StoredRow> rows;
    std::shared_ptr<StreamLog> log;
  };

  std::map<std::string, Table> tables_;  // lowercase name -> table
};

// An ICatalog over registered schemas.
class MapCatalog : public domain::ICatalog {
 public:
  // Registers a schema, replacing one with the same name (case-insensitive,
  // keeping its registration position).
  void Add(domain::TableSchema schema) {
    const std::string lowered = domain::ToLower(schema.name);
    for (domain::TableSchema& existing : schemas_) {
      if (domain::ToLower(existing.name) != lowered) {
        continue;
      }
      existing = std::move(schema);
      return;
    }
    schemas_.push_back(std::move(schema));
  }

  std::optional<domain::TableSchema> FindTable(
      const std::string& name) const override {
    const std::string lowered = domain::ToLower(name);
    for (const domain::TableSchema& schema : schemas_) {
      if (domain::ToLower(schema.name) == lowered) {
        return schema;
      }
    }
    return std::nullopt;
  }

  std::vector<std::string> TableNames() const override {
    std::vector<std::string> names;
    for (const domain::TableSchema& schema : schemas_) {
      names.push_back(schema.name);
    }
    return names;
  }

 private:
  std::vector<domain::TableSchema> schemas_;
};

}  // namespace strata::testing

#endif  // STRATA_TESTS_TEST_PAGED_ROW_FAKES_H_
