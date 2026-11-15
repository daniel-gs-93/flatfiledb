#ifndef STRATA_PAGED_DATABASE_H_
#define STRATA_PAGED_DATABASE_H_

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "strata/domain/value.h"
#include "strata/paged/buffer_manager.h"
#include "strata/paged/catalog.h"
#include "strata/paged/page_store.h"
#include "strata/paged/paged_types.h"
#include "strata/paged/table_heap.h"

// The paged engine's storage layer as one object: the database file, the
// buffer pool in front of it, the catalog, and the table heaps (design
// document 2.1, 3.3). Resource-aware. A Database owns the IPageStore,
// BufferManager, Catalog and TableHeaps through unique_ptr; it can be moved
// (the references between the members point at heap objects whose addresses do
// not change) but not copied.
//
// Writes need the caller to ensure a single writer at a time (QueryService
// holds a database-level reader-writer lock for this); reads may run from
// several threads.

namespace strata::paged {

struct DatabaseOptions {
  std::size_t pool_frames = 256;
  ReplacementPolicyKind policy = ReplacementPolicyKind::kLru;
};

class Database {
 public:
  // Opens directory/paged.db, creating it (with the file header and the two
  // system tables) when it does not exist or is empty. kIo: the directory is
  // missing or not a directory, or the file cannot be opened. kCorrupt: the
  // file fails the checks of design document 2.9.
  static PageResult<Database> Open(const std::filesystem::path& directory,
                                   const DatabaseOptions& options);

  // As Open, over a store the caller hands over. A test that must inject
  // failures after opening keeps a non-owning pointer to its MemoryPageStore;
  // the pointer stays valid until the Database is destroyed.
  static PageResult<Database> OpenWithStore(std::unique_ptr<IPageStore> store,
                                            const DatabaseOptions& options);

  Database(Database&& other) noexcept = default;
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  Database& operator=(Database&&) = delete;
  // Writes the dirty pages if Close was not called; a failure cannot be
  // returned, so it is printed to stderr.
  ~Database();

  // The table's heap, opened on first use and cached. The pointer is not
  // owned and is valid until Close or destruction. kNotFound: no such table
  // (names are case-insensitive). kClosed: after Close. kCorrupt: a page of
  // the table fails its checks.
  PageResult<TableHeap*> Table(const std::string& name);

  // Catalog::CreateTable, refused with kClosed after Close.
  PageResult<domain::TableSchema> CreateTable(
      const domain::TableSchema& schema);

  Catalog& catalog() { return *catalog_; }
  BufferManager& buffer() { return *buffer_; }

  // Writes every dirty page and syncs the file. Calling it again after it
  // succeeded returns success. The caller must hold no PageGuard.
  Status Close();

 private:
  Database(std::unique_ptr<IPageStore> store,
           std::unique_ptr<BufferManager> buffer,
           std::unique_ptr<Catalog> catalog);

  std::unique_ptr<IPageStore> store_;
  std::unique_ptr<BufferManager> buffer_;
  std::unique_ptr<Catalog> catalog_;
  // Lower-cased table name to heap. Looking up a name is O(1); the mutex
  // serializes the opening of a heap, which several reader threads may request
  // at once. It is a unique_ptr so that Database stays movable.
  std::unordered_map<std::string, std::unique_ptr<TableHeap>> tables_;
  std::unique_ptr<std::mutex> tables_mutex_ = std::make_unique<std::mutex>();
  bool is_closed_ = false;
};

}  // namespace strata::paged

#endif  // STRATA_PAGED_DATABASE_H_
