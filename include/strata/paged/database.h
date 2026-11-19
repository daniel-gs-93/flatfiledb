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
#include "strata/paged/page_allocator.h"
#include "strata/paged/page_store.h"
#include "strata/paged/paged_types.h"
#include "strata/paged/table_heap.h"

// The paged engine's storage layer as one object: the database file, the
// buffer pool in front of it, the catalog, and the table heaps (design
// document 2.1, 3.1). Resource-aware. A Database owns the IPageStore,
// BufferManager, PageAllocator, Catalog and TableHeaps through unique_ptr; it
// can be moved (the references between the members point at heap objects whose
// addresses do not change) but not copied.
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
  // file fails the checks of design document 2.15.
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
  // owned and is valid until Close, destruction, or AddColumn or DropTable of
  // that table. kNotFound: no such table
  // (names are case-insensitive). kClosed: after Close. kCorrupt: a page of
  // the table fails its checks.
  PageResult<TableHeap*> Table(const std::string& name);

  // Catalog::CreateTable, refused with kClosed after Close.
  PageResult<domain::TableSchema> CreateTable(
      const domain::TableSchema& schema);

  // ALTER TABLE ADD COLUMN (design document 2.6, 3.4, ADR-0045): checks the
  // new catalog row and that every row still fits in a tuple with the
  // default appended (no page is written when a check fails), copies the rows
  // into a new page chain, points the catalog at it, and releases the old
  // chain. Returns the schema after the change. kNotFound: no such table.
  // kAlreadyExists: the column exists. kInvalidArgument: no DEFAULT of the
  // column's type, or the catalog row does not fit. kTooLarge: a row would
  // exceed kMaxTupleSize. kCorrupt: a page of the table fails its checks.
  // kClosed: after Close. When a step before the switch fails the new chain
  // is released and the table is as it was; when only the release of the old
  // chain fails the change is applied and the message ends with `; statement
  // was applied but its pages were not reclaimed`. The caller ensures a
  // single writer and no reader of the table.
  PageResult<domain::TableSchema> AddColumn(const std::string& table,
                                            const domain::ColumnDef& column);

  // DROP TABLE (design document 2.6, 3.4, ADR-0046): removes the table from
  // the catalog and puts its page chain on the free list. kNotFound,
  // kCorrupt and kClosed as for AddColumn. When a catalog step fails the rows
  // already deleted are inserted again; when only the release fails the table
  // is gone and the message ends with `; statement was applied but its pages
  // were not reclaimed`. The caller ensures a single writer and no reader of
  // the table.
  Status DropTable(const std::string& table);

  Catalog& catalog() { return *catalog_; }
  BufferManager& buffer() { return *buffer_; }
  PageAllocator& allocator() { return *allocator_; }

  // Writes every dirty page and syncs the file. Calling it again after it
  // succeeded returns success. The caller must hold no PageGuard.
  Status Close();

 private:
  Database(std::unique_ptr<IPageStore> store,
           std::unique_ptr<BufferManager> buffer,
           std::unique_ptr<PageAllocator> allocator,
           std::unique_ptr<Catalog> catalog);

  // Table with tables_mutex_ already held.
  PageResult<TableHeap*> TableLocked(const std::string& name);

  std::unique_ptr<IPageStore> store_;
  std::unique_ptr<BufferManager> buffer_;
  std::unique_ptr<PageAllocator> allocator_;
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
