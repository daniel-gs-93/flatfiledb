#ifndef FLATFILEDB_BOOTSTRAP_H_
#define FLATFILEDB_BOOTSTRAP_H_

#include <cstddef>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "flatfiledb/application.h"

namespace flatfiledb::paged {
class PagedEngine;  // only the composition root names it (bootstrap.cpp)
}  // namespace flatfiledb::paged

namespace flatfiledb::bootstrap {

// The storage engine behind an opened database.
enum class EngineKind { kFlatFile, kPaged };

// A table of an opened database, its number of rows, and the lines of its file
// that were rejected on loading.
struct TableCount {
  std::string name;
  std::size_t row_count;
  std::vector<std::size_t> rejected_lines = {};
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
  bool is_created = false;         // this open created the directory
  EngineKind engine = EngineKind::kFlatFile;
  // Borrowed: the PagedEngine inside `service`, valid until `service` is
  // destroyed or replaced; null for the flat-file engine. The caller never
  // dereferences it (it is an incomplete type here); Bootstrap uses it to
  // flush the current database before reopening the same directory.
  paged::PagedEngine* paged_engine = nullptr;
  // The `note:` lines to print after the `loaded` line, one per element, when
  // the directory also holds the other engine's data (design document 3.4).
  std::vector<std::string> notes = {};
};

// The composition root shared by every entry point. Creates the directory when
// it does not exist and its parent does (one level), locks it, builds the CSV
// repository, the CSV source for `.import` and the flat-file engine, loads the
// tables, and wraps them in a QueryService. Returns the opened database, or the
// reason it cannot be opened (docs/DesignDocument.md section 3.5).
//
// `current` is the database the caller has open, or null. Opening the directory
// that `current` already holds reuses its lock: a second flock on the same file
// from this process would be refused. The caller owns the result; `current` is
// not modified, so a failed open leaves the caller's database untouched.
//
// When the loaded database has no table and the directory holds paged.db, the
// result carries the note that `.engine paged` opens it.
std::variant<OpenedDatabase, std::string> OpenFlatFileDatabase(
    const std::string& directory, bool is_parallel,
    const OpenedDatabase* current);

// As OpenFlatFileDatabase, over the paged engine, with the default storage
// options (256 frames, Least Recently Used replacement); see
// docs/PagedExecutionDesignDocument.md sections 2.8 and 3.3. The directory is
// always locked, because the engine writes paged.db: a directory in which the
// lock file cannot be created for lack of permission is refused with
// "directory is not writable". There is no load phase, so no parallel flag.
//
// When `current` is a paged database on the same directory, its dirty pages are
// written out (PagedEngine::Flush) before the file is read again; the open is
// aborted with the description when that fails. That Flush runs without the
// QueryService lock, so the caller must not run a statement on `current` at
// the same time (the single-threaded CLI never does). A paged.db that fails its
// checks, or a table that cannot be read in full, fails the whole open with a
// description that starts with "corrupt: ". `current` is not otherwise
// modified. The result's `tables` hold the row count of every table, and when
// there is no table and the directory holds catalog.csv, the result carries the
// note that `.engine flatfile` opens it.
std::variant<OpenedDatabase, std::string> OpenPagedDatabase(
    const std::string& directory, const OpenedDatabase* current);

}  // namespace flatfiledb::bootstrap

#endif  // FLATFILEDB_BOOTSTRAP_H_
