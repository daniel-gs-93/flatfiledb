#ifndef FLATFILEDB_BOOTSTRAP_H_
#define FLATFILEDB_BOOTSTRAP_H_

#include <cstddef>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "flatfiledb/application.h"

namespace flatfiledb::bootstrap {

// A table of an opened database and its number of rows.
struct TableCount {
  std::string name;
  std::size_t row_count;
};

// An opened database: the service that every entry point executes statements
// on, and what the caller needs to report and to open another database.
struct OpenedDatabase {
  std::unique_ptr<application::QueryService> service;
  std::string directory;  // the normalized path of the database directory
  // Keeps the directory lock; the lock is released when the last owner is
  // destroyed. Empty when the directory was not writable and no lock was taken.
  std::shared_ptr<void> lock;
  std::vector<TableCount> tables;  // in catalog order
};

// The composition root shared by every entry point. Locks the database
// directory, builds the CSV repository and the flat-file engine, loads the
// tables, and wraps the engine in a QueryService. Returns the opened database,
// or the reason it cannot be opened (docs/DesignDocument.md section 3.5).
//
// `current` is the database the caller has open, or null. Opening the directory
// that `current` already holds reuses its lock: a second flock on the same file
// from this process would be refused. The caller owns the result; `current` is
// not modified, so a failed open leaves the caller's database untouched.
std::variant<OpenedDatabase, std::string> OpenFlatFileDatabase(
    const std::string& directory, bool is_parallel,
    const OpenedDatabase* current);

}  // namespace flatfiledb::bootstrap

#endif  // FLATFILEDB_BOOTSTRAP_H_
