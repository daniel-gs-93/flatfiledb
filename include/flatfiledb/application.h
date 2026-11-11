#ifndef FLATFILEDB_APPLICATION_H_
#define FLATFILEDB_APPLICATION_H_

#include <memory>
#include <vector>

#include "flatfiledb/domain/ast.h"
#include "flatfiledb/domain/engine.h"
#include "flatfiledb/domain/import.h"
#include "flatfiledb/domain/value.h"

namespace flatfiledb::application {

// Executes one statement: binds it against the engine's catalog, then hands
// the plan to the engine. Owns the engine. Imports only the domain.
class QueryService {
 public:
  // csv_source serves ImportCsv; without it ImportCsv reports kUnsupported.
  explicit QueryService(
      std::unique_ptr<domain::IEngine> engine,
      std::unique_ptr<domain::ICsvSource> csv_source = nullptr);

  // Borrows statement for the duration of the call.
  domain::ExecutionResult Execute(const domain::Statement& statement);

  // The schema of every table of the engine's catalog, in registration order.
  // The data of the `.tables` and `.schema` commands.
  std::vector<domain::TableSchema> DescribeCatalog() const;

  // Imports an external CSV file as a new table (docs/DesignDocument.md
  // sections 2.7 and 3.8): reads it through the CSV source, infers the table,
  // and has the engine create and write it in one step. Returns a
  // CommandComplete whose tag is "IMPORT <table> (imported <n> rows[, skipped
  // <m>])" and whose notes describe the skipped rows, or an ExecutionError.
  domain::ExecutionResult ImportCsv(const domain::ImportRequest& request);

 private:
  std::unique_ptr<domain::IEngine> engine_;
  std::unique_ptr<domain::ICsvSource> csv_source_;
};

}  // namespace flatfiledb::application

#endif  // FLATFILEDB_APPLICATION_H_
