#ifndef FLATFILEDB_FLAT_FILE_ENGINE_H_
#define FLATFILEDB_FLAT_FILE_ENGINE_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "flatfiledb/domain/engine.h"
#include "flatfiledb/flat_file_executor.h"
#include "flatfiledb/infrastructure.h"

namespace flatfiledb::infrastructure {

// ICatalog over a TableStore. Borrows the store; the engine that owns both
// keeps them alive together.
class FlatFileCatalog : public domain::ICatalog {
 public:
  explicit FlatFileCatalog(const TableStore& store) : store_(store) {}

  std::optional<domain::TableSchema> FindTable(
      const std::string& name) const override;
  std::vector<std::string> TableNames() const override;

 private:
  const TableStore& store_;
};

// A table of a loaded database and its number of rows.
struct TableCount {
  std::string name;
  std::size_t row_count;
};

// The outcome of FlatFileEngine::Load. error is empty on success; tables lists
// every table in catalog order.
struct LoadResult {
  bool is_success;
  std::string error;
  std::vector<TableCount> tables;
};

// Engine over a database directory. Owns the in-memory tables and the
// repository. Executes plans with the pure functions in flat_file_executor.h,
// persists the effect through the repository, and replaces the in-memory
// tables only after persisting succeeded.
class FlatFileEngine : public domain::IEngine {
 public:
  // Precondition: repository is not null.
  explicit FlatFileEngine(std::unique_ptr<IFlatFileRepository> repository);

  // catalog_ refers to store_, so a copy or move would leave it dangling.
  FlatFileEngine(const FlatFileEngine&) = delete;
  FlatFileEngine& operator=(const FlatFileEngine&) = delete;

  // Loads every table of the catalog and registers it. On failure the engine
  // keeps its previous tables and error says why.
  LoadResult Load(bool is_parallel);

  const domain::ICatalog& Catalog() const override;
  domain::ExecutionResult Execute(const domain::PlanNode& plan) override;

 private:
  std::unique_ptr<IFlatFileRepository> repository_;
  TableStore store_;
  FlatFileCatalog catalog_;
};

}  // namespace flatfiledb::infrastructure

#endif  // FLATFILEDB_FLAT_FILE_ENGINE_H_
