#include "strata/flat_file_engine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// Resource-aware: this file is the only place that calls the repository. Plan
// execution lives in flat_file_executor.cpp as pure functions; the engine
// persists the effect first and replaces the in-memory tables only after the
// repository reported success, so a failed write leaves memory untouched.

namespace strata::infrastructure {

namespace {

void RegisterTable(TableData table, TableStore* store) {
  std::string key = domain::ToLower(table.schema.name);
  store->order.push_back(key);
  store->tables.emplace(std::move(key), std::move(table));
}

TableStore BuildStore(std::vector<LoadedTable> loaded) {
  TableStore store;
  for (LoadedTable& table : loaded) {
    RegisterTable(TableData{std::move(table.schema), std::move(table.rows)},
                  &store);
  }
  return store;
}

// Appends the rows on disk in one call, then in memory. Takes ownership of the
// rows.
domain::ExecutionResult ApplyInsert(IFlatFileRepository& repository,
                                    TableStore& store,
                                    const std::string& table_name,
                                    std::vector<domain::Row> rows) {
  auto entry = store.tables.find(domain::ToLower(table_name));
  if (entry == store.tables.end()) {
    return domain::ExecutionError{domain::ErrorKind::kBind,
                                  "unknown table: " + table_name};
  }
  TableData& table = entry->second;
  std::optional<std::string> failure =
      repository.AppendRows(table.schema, rows);
  if (failure) {
    return domain::ExecutionError{domain::ErrorKind::kIo, std::move(*failure)};
  }
  const std::size_t appended_count = rows.size();
  table.rows.insert(table.rows.end(), std::make_move_iterator(rows.begin()),
                    std::make_move_iterator(rows.end()));
  return domain::AffectedRows{appended_count};
}

// One table of an UPDATE or DELETE that is about to be committed: the in-memory
// table to replace and the new rows, both borrowed from the caller.
struct PendingSwap {
  TableData* table;
  std::vector<domain::Row>* new_rows;
};

// Rewrites every changed table in one Commit call, then swaps the rows
// into memory. The writes follow registration order so the call is
// deterministic. After a successful commit the new rows are moved out of
// mutation.changed_tables, which is left moved-from.
//
// The mutation comes from PlanMutation, so every changed key names a registered
// table. A violation of that is reported before anything is written, which
// keeps the memory and the files unchanged.
domain::ExecutionResult ApplyRewrite(IFlatFileRepository& repository,
                                     TableStore& store,
                                     MutationPlan& mutation) {
  if (mutation.changed_tables.empty()) {
    return domain::AffectedRows{mutation.affected};
  }

  std::vector<TableWrite> writes;
  std::vector<PendingSwap> swaps;
  for (const std::string& key : store.order) {
    auto changed = mutation.changed_tables.find(key);
    if (changed == mutation.changed_tables.end()) {
      continue;
    }
    auto entry = store.tables.find(key);
    if (entry == store.tables.end()) {
      return domain::ExecutionError{
          domain::ErrorKind::kRuntime,
          "internal error: table " + key + " is registered but missing"};
    }
    writes.push_back(TableWrite{entry->second.schema, changed->second});
    swaps.push_back(PendingSwap{&entry->second, &changed->second});
  }
  if (swaps.size() != mutation.changed_tables.size()) {
    return domain::ExecutionError{
        domain::ErrorKind::kRuntime,
        "internal error: a changed table is not registered"};
  }

  CommitSet set;
  set.writes = std::move(writes);
  std::optional<std::string> failure = repository.Commit(set);
  if (failure) {
    return domain::ExecutionError{domain::ErrorKind::kIo, std::move(*failure)};
  }
  for (PendingSwap& swap : swaps) {
    swap.table->rows = std::move(*swap.new_rows);
  }
  return domain::AffectedRows{mutation.affected};
}

// The schemas of the catalog after the change, in registration order.
std::vector<domain::TableSchema> CatalogAfter(const TableStore& store,
                                              const SchemaChange& change) {
  std::vector<domain::TableSchema> schemas;
  for (const std::string& key : store.order) {
    if (key == change.table && change.kind == SchemaChangeKind::kDrop) {
      continue;
    }
    if (key == change.table && change.kind == SchemaChangeKind::kAddColumn) {
      schemas.push_back(change.data->schema);
      continue;
    }
    const TableData* table = store.Find(key);
    if (table != nullptr) {
      schemas.push_back(table->schema);
    }
  }
  if (change.kind == SchemaChangeKind::kCreate) {
    schemas.push_back(change.data->schema);
  }
  return schemas;
}

// A table the change creates without a CSV, or rewrites, is written with a
// header from its schema; an adopted CSV stays as it is.
CommitSet BuildCommitSet(const TableStore& store, const SchemaChange& change) {
  CommitSet set;
  set.catalog = CatalogAfter(store, change);
  switch (change.kind) {
    case SchemaChangeKind::kCreate:
      if (!change.is_adopted) {
        set.writes.push_back(
            TableWrite{change.data->schema, change.data->rows, true});
      }
      break;
    case SchemaChangeKind::kAddColumn:
      set.writes.push_back(
          TableWrite{change.data->schema, change.data->rows, true});
      break;
    case SchemaChangeKind::kDrop:
      set.drops.push_back(change.table);
      break;
  }
  return set;
}

void ApplyToStore(SchemaChange& change, TableStore* store) {
  switch (change.kind) {
    case SchemaChangeKind::kCreate:
      RegisterTable(std::move(*change.data), store);
      break;
    case SchemaChangeKind::kAddColumn: {
      auto entry = store->tables.find(change.table);
      if (entry != store->tables.end()) {
        entry->second = std::move(*change.data);
      }
      break;
    }
    case SchemaChangeKind::kDrop:
      store->tables.erase(change.table);
      std::erase(store->order, change.table);
      break;
  }
}

// CREATE TABLE asks the repository for the table's existing CSV first, so that
// the rows can be adopted. The change is committed together with catalog.csv;
// the in-memory tables change only after the commit succeeded.
domain::ExecutionResult ApplySchemaChange(IFlatFileRepository& repository,
                                          TableStore& store,
                                          const domain::PlanNode& plan) {
  std::optional<std::vector<domain::Row>> existing_rows;
  if (const auto* create = std::get_if<domain::CreateTableNode>(&plan.node)) {
    InspectOutcome inspected = repository.InspectTable(create->schema);
    if (auto* error = std::get_if<std::string>(&inspected)) {
      return domain::ExecutionError{domain::ErrorKind::kIo, std::move(*error)};
    }
    existing_rows =
        std::move(std::get<std::optional<std::vector<domain::Row>>>(inspected));
  }
  SchemaChangeOutcome outcome = PlanSchemaChange(plan, store, existing_rows);
  if (auto* error = std::get_if<domain::ExecutionError>(&outcome)) {
    return std::move(*error);
  }
  // The error alternative was ruled out above.
  SchemaChange& change = std::get<SchemaChange>(outcome);
  std::optional<std::string> failure =
      repository.Commit(BuildCommitSet(store, change));
  if (failure) {
    return domain::ExecutionError{domain::ErrorKind::kIo, std::move(*failure)};
  }
  ApplyToStore(change, &store);
  return domain::CommandComplete{std::move(change.tag)};
}

bool IsSchemaChangeNode(const domain::PlanNode& plan) {
  return std::holds_alternative<domain::CreateTableNode>(plan.node) ||
         std::holds_alternative<domain::AddColumnNode>(plan.node) ||
         std::holds_alternative<domain::DropTableNode>(plan.node) ||
         std::holds_alternative<domain::ImportTableNode>(plan.node);
}

bool IsWriteNode(const domain::PlanNode& plan) {
  return std::holds_alternative<domain::InsertNode>(plan.node) ||
         std::holds_alternative<domain::UpdateNode>(plan.node) ||
         std::holds_alternative<domain::DeleteNode>(plan.node);
}

}  // namespace

std::optional<domain::TableSchema> FlatFileCatalog::FindTable(
    const std::string& name) const {
  const TableData* table = store_.Find(name);
  if (table == nullptr) {
    return std::nullopt;
  }
  return table->schema;
}

std::vector<std::string> FlatFileCatalog::TableNames() const {
  return store_.order;
}

FlatFileEngine::FlatFileEngine(std::unique_ptr<IFlatFileRepository> repository)
    : repository_(std::move(repository)), catalog_(store_) {}

LoadResult FlatFileEngine::Load(bool is_parallel) {
  LoadOutcome outcome = repository_->Load(is_parallel);
  if (auto* error = std::get_if<std::string>(&outcome)) {
    return LoadResult{false, std::move(*error), {}};
  }
  // The error alternative was ruled out above.
  std::vector<LoadedTable>& tables =
      std::get<std::vector<LoadedTable>>(outcome);
  std::vector<TableCount> counts;
  for (const LoadedTable& table : tables) {
    counts.push_back(
        TableCount{table.schema.name, table.rows.size(), table.rejected_lines});
  }

  // The store is replaced in one step, after it is complete.
  TableStore loaded = BuildStore(std::move(tables));
  store_ = std::move(loaded);
  return LoadResult{true, std::string(), std::move(counts)};
}

const domain::ICatalog& FlatFileEngine::Catalog() const { return catalog_; }

domain::ExecutionResult FlatFileEngine::Execute(const domain::PlanNode& plan) {
  if (IsSchemaChangeNode(plan)) {
    return ApplySchemaChange(*repository_, store_, plan);
  }
  if (!IsWriteNode(plan)) {
    return RunQuery(plan, store_);
  }

  MutationOutcome outcome = PlanMutation(plan, store_);
  if (auto* error = std::get_if<domain::ExecutionError>(&outcome)) {
    return std::move(*error);
  }
  // The error alternative was ruled out above.
  MutationPlan& mutation = std::get<MutationPlan>(outcome);
  if (mutation.appended) {
    return ApplyInsert(*repository_, store_, mutation.table,
                       std::move(*mutation.appended));
  }
  return ApplyRewrite(*repository_, store_, mutation);
}

}  // namespace strata::infrastructure
