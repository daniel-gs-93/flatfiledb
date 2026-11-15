#ifndef STRATA_PAGED_TABLE_HEAP_H_
#define STRATA_PAGED_TABLE_HEAP_H_

#include <cstdint>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "strata/domain/value.h"
#include "strata/paged/buffer_manager.h"
#include "strata/paged/paged_types.h"
#include "strata/paged/slotted_page.h"

// One table as a chain of heap-data pages (design document 2.5): the rows are
// stored as tuples in slotted pages, with no order. A row is addressed by its
// Rid. Resource-aware: every page is reached through a BufferManager and held
// by a PageGuard.
//
// Concurrency contract: Insert, Update and Delete need the caller to ensure a
// single writer at a time. Get and Scan may run with other readers and with
// that one writer; each page is consistent when it is read, but there is no
// consistent snapshot across pages.

namespace strata::paged {

struct ScanItem {
  Rid rid;
  domain::Row row;
};

struct PageCapacity {
  PageId page_id;
  std::uint16_t capacity;
};

class TableHeap;

// Walks the page chain of a TableHeap one page at a time: the rows of a page
// are decoded under a shared latch and the latch is released before the rows
// are returned, so memory is bounded by the rows of one page. The TableHeap
// must outlive the scan.
class HeapScan {
 public:
  // The next row in chain order, then slot order within a page; the optional
  // is empty when there are no more rows. After an error the scan is over.
  PageResult<std::optional<ScanItem>> Next();

 private:
  friend class TableHeap;
  HeapScan(const TableHeap* heap, PageId first_page)
      : heap_(heap), next_page_(first_page) {}

  // Decodes the live tuples of page_id into rows_ and advances next_page_.
  Status LoadPage(PageId page_id);

  const TableHeap* heap_;
  PageId next_page_;
  std::vector<ScanItem> rows_;
  std::size_t position_ = 0;
  bool is_done_ = false;
};

class TableHeap {
 public:
  // Allocates and formats the first page of a new table and returns its page
  // number. The first page never changes.
  static PageResult<PageId> Create(BufferManager& buffer,
                                   std::uint32_t table_id);

  // Opens an existing table: walks the whole chain, checks every page, and
  // rebuilds the free-space map and the tail. kCorrupt: a page is not a heap
  // page of this table, or the chain has a cycle. buffer must outlive the
  // TableHeap.
  static PageResult<TableHeap> Open(BufferManager& buffer,
                                    domain::TableSchema schema,
                                    std::uint32_t table_id, PageId first_page);

  // The row is encoded first, so a rejected row touches no page.
  // kInvalidArgument: the row does not match the schema. kTooLarge: the tuple
  // is longer than kMaxTupleSize.
  PageResult<Rid> Insert(const domain::Row& row);

  // The row is a copy. kNotFound: the slot is dead or out of range, or the
  // page is beyond the store. kInvalidArgument: the page is not a heap page of
  // this table.
  PageResult<domain::Row> Get(Rid rid) const;

  // The row keeps its Rid. kNoSpace: the new tuple does not fit in the page;
  // the row is unchanged and the caller deletes and inserts again.
  Status Update(Rid rid, const domain::Row& row);

  Status Delete(Rid rid);

  HeapScan Scan() const { return HeapScan(this, first_page_); }

  PageId first_page() const { return first_page_; }
  PageId tail_page() const { return tail_page_; }
  std::uint32_t table_id() const { return table_id_; }
  const domain::TableSchema& schema() const { return schema_; }

  // The free-space map as (page id, capacity) pairs in page id order.
  std::vector<PageCapacity> FreeSpaceSnapshot() const;

 private:
  friend class HeapScan;

  TableHeap(BufferManager& buffer, domain::TableSchema schema,
            std::uint32_t table_id, PageId first_page);

  // The page that holds a heap row of this table, or the error to return.
  Status CheckOwnership(SlottedPageView view, PageErrorCode code) const;

  // The page with the smallest capacity that is at least size, lowest page
  // id first. O(log P) for P pages.
  std::optional<PageId> FindPageWithRoom(std::size_t size) const;
  void SetCapacity(PageId page_id, std::uint16_t capacity);

  // Appends a page after the tail, holding the tail's exclusive latch while
  // the new page is linked in (tail first, new page second).
  PageResult<Rid> InsertIntoNewPage(ByteView tuple);

  BufferManager& buffer_;
  domain::TableSchema schema_;
  std::uint32_t table_id_;
  PageId first_page_;
  PageId tail_page_;
  // Free-space map: capacity-ordered set for "smallest page that fits" in
  // O(log P), and a map from page id to capacity for updates in O(log P).
  // Only the writer touches them, and they are never saved (ADR-0031).
  std::set<std::pair<std::uint16_t, PageId>> by_capacity_;
  std::unordered_map<PageId, std::uint16_t> capacity_of_page_;
};

}  // namespace strata::paged

#endif  // STRATA_PAGED_TABLE_HEAP_H_
