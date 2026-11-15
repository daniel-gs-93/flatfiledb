#ifndef STRATA_DOMAIN_IMPORT_H_
#define STRATA_DOMAIN_IMPORT_H_

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "strata/domain/engine.h"
#include "strata/domain/value.h"

// The pure part of `.import` (docs/DesignDocument.md section 2.7): the request,
// the lines of an external CSV file as text cells, and the inference of a table
// from them.

namespace strata::domain {

struct ImportRequest {
  std::string file_path;
  std::string table;        // empty: the file name without its extension
  std::string primary_key;  // empty: the column named id
  // Column names (any case) with the type to use instead of the inferred one.
  std::vector<std::pair<std::string, DataType>> column_types;
  bool should_skip_bad_rows = false;
};

// One non-empty line of the file: its line number (from 1, counting empty
// lines) and the cells; fields is empty when the quoting is malformed.
struct CsvLine {
  std::size_t number;
  std::optional<std::vector<std::string>> fields;
};

// Reads an external CSV file. The first CsvLine is the header. Implemented by
// the infrastructure; the string is the reason the file cannot be read.
class ICsvSource {
 public:
  virtual ~ICsvSource() = default;
  virtual std::variant<std::vector<CsvLine>, std::string> Read(
      const std::string& path) = 0;
};

// The table an import creates.
struct ImportPlan {
  TableSchema schema;
  std::vector<Row> rows;
  std::size_t skipped_count = 0;
  // Lines to print after the result: one per skipped row (at most 10) and a
  // closing "... and <k> more skipped" when there are more.
  std::vector<std::string> notes;
};

// Checks the request against the file's lines and the catalog, infers the
// column types, converts the rows and chooses the primary key. A failure is a
// kBind error whose message names the line or the option at fault. Pure.
std::variant<ImportPlan, ExecutionError> InferImport(
    const ImportRequest& request, const std::vector<CsvLine>& lines,
    const ICatalog& catalog);

}  // namespace strata::domain

#endif  // STRATA_DOMAIN_IMPORT_H_
