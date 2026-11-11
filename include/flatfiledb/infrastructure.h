#ifndef FLATFILEDB_INFRASTRUCTURE_H_
#define FLATFILEDB_INFRASTRUCTURE_H_

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "flatfiledb/domain/value.h"

namespace flatfiledb::infrastructure {

// One table as loaded from its CSV file.
struct LoadedTable {
  domain::TableSchema schema;
  std::vector<domain::Row> rows;
};

// The tables of a database directory in catalog order, or the reason the
// database cannot be loaded.
using LoadOutcome = std::variant<std::vector<LoadedTable>, std::string>;

// The rows of a table's existing CSV file (empty optional when there is no
// file), or the reason it cannot be read.
using InspectOutcome =
    std::variant<std::optional<std::vector<domain::Row>>, std::string>;

// One table to be rewritten: its schema and every row it will contain. The
// file keeps its first non-empty line as the header, unless
// should_regenerate_header is set (ALTER TABLE ADD COLUMN), which writes the
// header from the schema.
struct TableWrite {
  domain::TableSchema schema;
  std::vector<domain::Row> rows;
  bool should_regenerate_header = false;
};

// Everything one commit changes: catalog.csv (when set, with the complete list
// of schemas in catalog order), tables to rewrite or create, and tables whose
// file is deleted.
struct CommitSet {
  std::optional<std::vector<domain::TableSchema>> catalog;
  std::vector<TableWrite> writes;
  std::vector<std::string> drops;  // table names
};

// Persistence port used by FlatFileEngine. Failures are reported as a
// description; std::nullopt means success where the result is optional.
class IFlatFileRepository {
 public:
  virtual ~IFlatFileRepository() = default;

  // Reads catalog.csv and every table it lists, serially or with one worker
  // thread per table (at most the hardware concurrency). The rules are those of
  // docs/DesignDocument.md section 2.5; a file that cannot be read fails the
  // whole load and no partial result is returned.
  virtual LoadOutcome Load(bool is_parallel) = 0;

  // Reads the table's CSV file under the loading rules without registering
  // anything; the header must match the schema.
  virtual InspectOutcome InspectTable(const domain::TableSchema& schema) = 0;

  // Replaces, creates and deletes the files of the commit set as one unit
  // (staged commit, see docs/DesignDocument.md section 3.3). On failure every
  // file keeps its content from before the call, except when restoring also
  // fails, which the description states.
  virtual std::optional<std::string> Commit(const CommitSet& set) = 0;

  // Appends one row to the end of the table's file.
  virtual std::optional<std::string> AppendRow(
      const domain::TableSchema& schema, const domain::Row& row) = 0;
};

// Loads and persists a database directory: catalog.csv and one CSV file per
// table, named after the lower-case table name. The only class that touches the
// table files, or starts threads to load them.
class CsvRepository : public IFlatFileRepository {
 public:
  explicit CsvRepository(std::string directory);

  LoadOutcome Load(bool is_parallel) override;
  InspectOutcome InspectTable(const domain::TableSchema& schema) override;
  std::optional<std::string> Commit(const CommitSet& set) override;
  std::optional<std::string> AppendRow(const domain::TableSchema& schema,
                                       const domain::Row& row) override;

 private:
  std::string directory_;
};

}  // namespace flatfiledb::infrastructure

#endif  // FLATFILEDB_INFRASTRUCTURE_H_
