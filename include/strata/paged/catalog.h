#ifndef STRATA_PAGED_CATALOG_H_
#define STRATA_PAGED_CATALOG_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "strata/domain/engine.h"
#include "strata/domain/value.h"
#include "strata/paged/buffer_manager.h"
#include "strata/paged/paged_types.h"
#include "strata/paged/table_heap.h"

// The schemas of the user tables, stored in two system tables inside the same
// database file (design document 2.6, ADR-0032): sys_tables (table id 1) and
// sys_columns (table id 2). The catalog reads them once into an in-memory
// cache, so FindTable is O(1) plus a copy of the schema.
//
// FindTable, TableNames, TableId and FirstPage may be called from several
// threads at once; the cache is guarded by a shared_mutex. CreateTable calls
// are serialized by a mutex and update the cache under the exclusive lock.

namespace strata::paged {

class Catalog : public domain::ICatalog {
 public:
  // The fixed schemas of the two system tables.
  static domain::TableSchema SysTablesSchema();
  static domain::TableSchema SysColumnsSchema();

  // The first pages of the system tables are read from the file header by the
  // caller and passed in. buffer must outlive the Catalog. Nothing is read
  // until Load.
  Catalog(BufferManager& buffer, PageId sys_tables_first_page,
          PageId sys_columns_first_page);

  // Reads and validates both system tables, including the target of every
  // foreign key, and the file header's next_table_id, and replaces the cache.
  // kCorrupt names the table and column; see design document 2.6 for the
  // checks.
  Status Load();

  // Adds a table: validates schema, writes the table's first page and its rows
  // in the system tables, advances next_table_id, and then updates the cache.
  // There is no transaction: when a step fails the rows already written are
  // deleted (best effort) and the first error is returned; the new table's
  // first page stays allocated. kAlreadyExists: the name is taken (compared
  // case-insensitively). kInvalidArgument: any other rule of 2.6 is violated.
  PageResult<domain::TableSchema> CreateTable(
      const domain::TableSchema& schema);

  // ICatalog: names are matched case-insensitively, and the schema is a copy.
  std::optional<domain::TableSchema> FindTable(
      const std::string& name) const override;
  // User tables only, in creation order.
  std::vector<std::string> TableNames() const override;

  std::optional<std::uint32_t> TableId(const std::string& name) const;
  std::optional<PageId> FirstPage(const std::string& name) const;

 private:
  struct Entry {
    std::uint32_t table_id;
    domain::TableSchema schema;
    PageId first_page;
  };

  // Undoes the rows of a failed CreateTable; returns the message to append to
  // the original error when the undo itself fails, else empty.
  std::string UndoRows(const std::vector<Rid>& table_rows,
                       const std::vector<Rid>& column_rows);

  BufferManager& buffer_;
  PageId sys_tables_first_page_;
  PageId sys_columns_first_page_;
  std::unique_ptr<TableHeap> sys_tables_;
  std::unique_ptr<TableHeap> sys_columns_;
  std::uint32_t next_table_id_ = 0;

  mutable std::shared_mutex cache_mutex_;
  // In creation order (ascending table id); and a map from lower-cased name to
  // the position in entries_ for O(1) lookup.
  std::vector<Entry> entries_;
  std::unordered_map<std::string, std::size_t> position_of_name_;

  std::mutex create_mutex_;
};

}  // namespace strata::paged

#endif  // STRATA_PAGED_CATALOG_H_
