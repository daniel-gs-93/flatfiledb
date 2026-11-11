#ifndef FLATFILEDB_FLAT_FILE_EXECUTOR_H_
#define FLATFILEDB_FLAT_FILE_EXECUTOR_H_

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "flatfiledb/domain/plan.h"
#include "flatfiledb/domain/value.h"

namespace flatfiledb::infrastructure {

struct TableData {
  domain::TableSchema schema;
  std::vector<domain::Row> rows;
};

// The in-memory tables of one FlatFileEngine.
struct TableStore {
  std::map<std::string, TableData> tables;  // key: lower-case table name
  std::vector<std::string> order;  // lower-case names, registration order

  // Case-insensitive; null when the table does not exist.
  const TableData* Find(const std::string& name) const {
    auto entry = tables.find(domain::ToLower(name));
    return entry == tables.end() ? nullptr : &entry->second;
  }
};

// What a successful INSERT, UPDATE or DELETE changes. Computing it does not
// modify the store: the engine persists it, then applies it.
struct MutationPlan {
  std::string table;                    // table named by the statement
  std::optional<domain::Row> appended;  // set for INSERT, otherwise empty
  // UPDATE and DELETE: lower-case table name to the complete new row set of
  // every table whose content changes (the target and any cascaded children).
  std::map<std::string, std::vector<domain::Row>> changed_tables;
  std::size_t affected;
};

using MutationOutcome = std::variant<MutationPlan, domain::ExecutionError>;

enum class SchemaChangeKind { kCreate, kAddColumn, kDrop };

// What a successful CREATE TABLE, ALTER TABLE ADD COLUMN or DROP TABLE changes.
// Computing it does not modify the store: the engine persists it, then applies
// it.
struct SchemaChange {
  SchemaChangeKind kind;
  std::string tag;    // the CommandComplete tag, e.g. "CREATE TABLE users"
  std::string table;  // lower-case name of the affected table
  // The table after the change: new for kCreate (with the adopted rows) and
  // kAddColumn (every row with the default appended); empty for kDrop.
  std::optional<TableData> data;
  bool is_adopted;  // kCreate: the table's CSV existed, so it stays as it is
};

using SchemaChangeOutcome = std::variant<SchemaChange, domain::ExecutionError>;

// Pure functions over the store. RunQuery executes a plan made of Scan,
// Filter, Join, Aggregate, Project and Sort nodes and returns a ResultSet, or
// ExecutionError{kUnsupported} for a join of more than two tables or
// {kRuntime} for INT overflow. PlanMutation handles InsertNode, UpdateNode and
// DeleteNode and applies the checks in docs/DesignDocument.md section 2.5.
domain::ExecutionResult RunQuery(const domain::PlanNode& plan,
                                 const TableStore& store);
MutationOutcome PlanMutation(const domain::PlanNode& plan,
                             const TableStore& store);

// Handles CreateTableNode, AddColumnNode and DropTableNode. existing_rows is,
// for a CreateTableNode, the rows of the table's CSV when that file exists
// (parsed but not de-duplicated); they must satisfy the declared primary key,
// UNIQUE columns and foreign keys, or the result is a kConstraintViolation
// (docs/DesignDocument.md section 2.5).
SchemaChangeOutcome PlanSchemaChange(
    const domain::PlanNode& plan, const TableStore& store,
    const std::optional<std::vector<domain::Row>>& existing_rows);

}  // namespace flatfiledb::infrastructure

#endif  // FLATFILEDB_FLAT_FILE_EXECUTOR_H_
