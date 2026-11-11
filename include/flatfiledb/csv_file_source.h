#ifndef FLATFILEDB_CSV_FILE_SOURCE_H_
#define FLATFILEDB_CSV_FILE_SOURCE_H_

#include <string>
#include <variant>
#include <vector>

#include "flatfiledb/domain/import.h"

namespace flatfiledb::infrastructure {

// Reads an external CSV file for `.import` (docs/DesignDocument.md section
// 2.7): one CsvLine per non-empty line, cells split by RFC 4180 rules, a
// leading UTF-8 BOM and each trailing '\r' removed. Line numbers count the
// empty lines. A line whose quoting is malformed has no fields. Fails with
// "cannot read <path>" when the path is not a readable regular file.
class FileCsvSource : public domain::ICsvSource {
 public:
  std::variant<std::vector<domain::CsvLine>, std::string> Read(
      const std::string& path) override;
};

}  // namespace flatfiledb::infrastructure

#endif  // FLATFILEDB_CSV_FILE_SOURCE_H_
