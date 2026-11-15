#ifndef STRATA_INFRASTRUCTURE_H_
#define STRATA_INFRASTRUCTURE_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "strata/domain/value.h"

namespace strata::infrastructure {

// One table as loaded from its CSV file.
struct LoadedTable {
  domain::TableSchema schema;
  std::vector<domain::Row> rows;
  // Line numbers (from 1, counting the header and empty lines) of the rows the
  // schema rejected; they are not loaded and a later rewrite drops them.
  std::vector<std::size_t> rejected_lines = {};
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

  // Appends the rows to the end of the table's file in one write: all of them
  // or, on failure, none. An empty vector changes nothing and succeeds.
  virtual std::optional<std::string> AppendRows(
      const domain::TableSchema& schema,
      const std::vector<domain::Row>& rows) = 0;
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
  std::optional<std::string> AppendRows(
      const domain::TableSchema& schema,
      const std::vector<domain::Row>& rows) override;

 private:
  // What a file looked like when it was loaded or last written by this
  // repository (docs/DesignDocument.md section 2.6, external modification).
  struct FileStamp {
    bool is_present = false;
    std::uintmax_t size = 0;
    std::filesystem::file_time_type modified = {};

    bool operator==(const FileStamp&) const = default;
  };

  FileStamp StampOf(const std::string& file_name) const;
  // The failure description for the first of file_names whose current stamp
  // differs from the recorded one; files without a record are not checked.
  std::optional<std::string> FindChangedFile(
      const std::vector<std::string>& file_names) const;
  void RecordStamps(const std::vector<std::string>& file_names);

  std::string directory_;
  std::map<std::string, FileStamp> stamps_;  // by file name
};

}  // namespace strata::infrastructure

#endif  // STRATA_INFRASTRUCTURE_H_
