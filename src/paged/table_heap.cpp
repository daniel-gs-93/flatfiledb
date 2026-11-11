#include "flatfiledb/paged/table_heap.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "flatfiledb/paged/slotted_page.h"
#include "flatfiledb/paged/tuple_codec.h"

namespace flatfiledb::paged {

namespace {

ByteView ViewOfBytes(const std::vector<std::uint8_t>& bytes) {
  return ByteView{bytes.data(), bytes.size()};
}

bool PageIdLess(const PageCapacity& left, const PageCapacity& right) {
  return left.page_id < right.page_id;
}

}  // namespace

PageResult<std::optional<ScanItem>> HeapScan::Next() {
  while (true) {
    if (position_ < rows_.size()) {
      return std::optional<ScanItem>(std::move(rows_[position_++]));
    }
    if (is_done_ || next_page_ == kInvalidPageId) {
      is_done_ = true;
      return std::optional<ScanItem>();
    }
    if (Status status = LoadPage(next_page_)) {
      is_done_ = true;
      return *status;
    }
  }
}

Status HeapScan::LoadPage(PageId page_id) {
  rows_.clear();
  position_ = 0;
  PageResult<PageGuard> fetched =
      heap_->buffer_.FetchPage(page_id, LatchMode::kShared);
  if (!fetched.ok()) {
    return fetched.error();
  }
  const PageGuard guard = std::move(fetched).value();
  const SlottedPageView view(guard.data());
  if (Status status = heap_->CheckOwnership(view, PageErrorCode::kCorrupt)) {
    return status;
  }
  for (std::uint16_t slot_id = 0; slot_id < view.slot_count(); ++slot_id) {
    PageResult<ByteView> bytes = view.Get(slot_id);
    if (!bytes.ok()) {
      if (bytes.error().code == PageErrorCode::kNotFound) {
        continue;
      }
      return bytes.error();
    }
    PageResult<domain::Row> row =
        TupleCodec::Decode(heap_->schema_, bytes.value());
    if (!row.ok()) {
      return row.error();
    }
    rows_.push_back(ScanItem{Rid{page_id, slot_id}, std::move(row).value()});
  }
  next_page_ = view.next_page_id();
  return std::nullopt;
}

TableHeap::TableHeap(BufferManager& buffer, domain::TableSchema schema,
                     std::uint32_t table_id, PageId first_page)
    : buffer_(buffer),
      schema_(std::move(schema)),
      table_id_(table_id),
      first_page_(first_page),
      tail_page_(first_page) {}

PageResult<PageId> TableHeap::Create(BufferManager& buffer,
                                     std::uint32_t table_id) {
  PageResult<PageGuard> created = buffer.NewPage();
  if (!created.ok()) {
    return created.error();
  }
  PageGuard guard = std::move(created).value();
  SlottedPage::Format(guard.mutable_data(), guard.page_id(), table_id);
  guard.MarkDirty();
  return guard.page_id();
}

PageResult<TableHeap> TableHeap::Open(BufferManager& buffer,
                                      domain::TableSchema schema,
                                      std::uint32_t table_id,
                                      PageId first_page) {
  TableHeap heap(buffer, std::move(schema), table_id, first_page);
  std::unordered_set<PageId> visited;
  PageId current = first_page;
  while (current != kInvalidPageId) {
    if (!visited.insert(current).second) {
      return MakeError(PageErrorCode::kCorrupt,
                       "the page chain of table " + heap.schema_.name +
                           " revisits page " + std::to_string(current));
    }
    PageResult<PageGuard> fetched =
        buffer.FetchPage(current, LatchMode::kShared);
    if (!fetched.ok()) {
      return fetched.error();
    }
    const PageGuard guard = std::move(fetched).value();
    const SlottedPageView view(guard.data());
    if (Status status = heap.CheckOwnership(view, PageErrorCode::kCorrupt)) {
      return *status;
    }
    heap.SetCapacity(current, view.InsertCapacity());
    heap.tail_page_ = current;
    current = view.next_page_id();
  }
  return PageResult<TableHeap>(std::move(heap));
}

Status TableHeap::CheckOwnership(SlottedPageView view,
                                 PageErrorCode code) const {
  if (view.page_type() != PageType::kHeapData || view.table_id() != table_id_) {
    return MakeError(code,
                     "page " + std::to_string(view.page_id()) +
                         " is not a heap page of table " + schema_.name +
                         " (page_type " +
                         std::to_string(static_cast<int>(view.page_type())) +
                         ", table_id " + std::to_string(view.table_id()) + ")");
  }
  return std::nullopt;
}

std::optional<PageId> TableHeap::FindPageWithRoom(std::size_t size) const {
  auto found = by_capacity_.lower_bound(
      std::make_pair(static_cast<std::uint16_t>(size), PageId{0}));
  if (found == by_capacity_.end()) {
    return std::nullopt;
  }
  return found->second;
}

void TableHeap::SetCapacity(PageId page_id, std::uint16_t capacity) {
  auto existing = capacity_of_page_.find(page_id);
  if (existing != capacity_of_page_.end()) {
    by_capacity_.erase(std::make_pair(existing->second, page_id));
    existing->second = capacity;
  } else {
    capacity_of_page_.emplace(page_id, capacity);
  }
  by_capacity_.emplace(capacity, page_id);
}

std::vector<PageCapacity> TableHeap::FreeSpaceSnapshot() const {
  std::vector<PageCapacity> snapshot;
  snapshot.reserve(capacity_of_page_.size());
  for (const auto& [page_id, capacity] : capacity_of_page_) {
    snapshot.push_back(PageCapacity{page_id, capacity});
  }
  std::ranges::sort(snapshot, PageIdLess);
  return snapshot;
}

PageResult<Rid> TableHeap::InsertIntoNewPage(ByteView tuple) {
  PageResult<PageGuard> tail_fetched =
      buffer_.FetchPage(tail_page_, LatchMode::kExclusive);
  if (!tail_fetched.ok()) {
    return tail_fetched.error();
  }
  PageGuard tail_guard = std::move(tail_fetched).value();
  PageResult<PageGuard> created = buffer_.NewPage();
  if (!created.ok()) {
    return created.error();
  }
  PageGuard new_guard = std::move(created).value();
  const PageId new_page_id = new_guard.page_id();
  SlottedPage::Format(new_guard.mutable_data(), new_page_id, table_id_);
  SlottedPage new_page(new_guard.mutable_data());
  PageResult<std::uint16_t> slot = new_page.Insert(tuple);
  if (!slot.ok()) {
    return slot.error();
  }
  new_guard.MarkDirty();
  SlottedPage tail_page(tail_guard.mutable_data());
  tail_page.set_next_page_id(new_page_id);
  tail_guard.MarkDirty();
  SetCapacity(new_page_id, new_page.InsertCapacity());
  tail_page_ = new_page_id;
  return Rid{new_page_id, slot.value()};
}

PageResult<Rid> TableHeap::Insert(const domain::Row& row) {
  PageResult<std::vector<std::uint8_t>> encoded =
      TupleCodec::Encode(schema_, row);
  if (!encoded.ok()) {
    return encoded.error();
  }
  const ByteView tuple = ViewOfBytes(encoded.value());
  for (int attempt = 0; attempt < 2; ++attempt) {
    const std::optional<PageId> candidate = FindPageWithRoom(tuple.size);
    if (!candidate.has_value()) {
      return InsertIntoNewPage(tuple);
    }
    PageResult<PageGuard> fetched =
        buffer_.FetchPage(*candidate, LatchMode::kExclusive);
    if (!fetched.ok()) {
      return fetched.error();
    }
    PageGuard guard = std::move(fetched).value();
    SlottedPage page(guard.mutable_data());
    PageResult<std::uint16_t> slot = page.Insert(tuple);
    if (slot.ok()) {
      guard.MarkDirty();
      SetCapacity(*candidate, page.InsertCapacity());
      return Rid{*candidate, slot.value()};
    }
    if (slot.error().code != PageErrorCode::kNoSpace) {
      return slot.error();
    }
    // The free-space map overestimated this page; correct it and choose again.
    SetCapacity(*candidate, page.InsertCapacity());
  }
  return MakeError(PageErrorCode::kCorrupt, "the free-space map of table " +
                                                schema_.name +
                                                " disagrees with its pages");
}

PageResult<domain::Row> TableHeap::Get(Rid rid) const {
  PageResult<PageGuard> fetched =
      buffer_.FetchPage(rid.page_id, LatchMode::kShared);
  if (!fetched.ok()) {
    return fetched.error();
  }
  const PageGuard guard = std::move(fetched).value();
  const SlottedPageView view(guard.data());
  if (Status status = CheckOwnership(view, PageErrorCode::kInvalidArgument)) {
    return *status;
  }
  PageResult<ByteView> bytes = view.Get(rid.slot_id);
  if (!bytes.ok()) {
    return bytes.error();
  }
  return TupleCodec::Decode(schema_, bytes.value());
}

Status TableHeap::Update(Rid rid, const domain::Row& row) {
  PageResult<std::vector<std::uint8_t>> encoded =
      TupleCodec::Encode(schema_, row);
  if (!encoded.ok()) {
    return encoded.error();
  }
  PageResult<PageGuard> fetched =
      buffer_.FetchPage(rid.page_id, LatchMode::kExclusive);
  if (!fetched.ok()) {
    return fetched.error();
  }
  PageGuard guard = std::move(fetched).value();
  if (Status status = CheckOwnership(SlottedPageView(guard.data()),
                                     PageErrorCode::kInvalidArgument)) {
    return status;
  }
  SlottedPage page(guard.mutable_data());
  if (Status status = page.Update(rid.slot_id, ViewOfBytes(encoded.value()))) {
    return status;
  }
  guard.MarkDirty();
  SetCapacity(rid.page_id, page.InsertCapacity());
  return std::nullopt;
}

Status TableHeap::Delete(Rid rid) {
  PageResult<PageGuard> fetched =
      buffer_.FetchPage(rid.page_id, LatchMode::kExclusive);
  if (!fetched.ok()) {
    return fetched.error();
  }
  PageGuard guard = std::move(fetched).value();
  if (Status status = CheckOwnership(SlottedPageView(guard.data()),
                                     PageErrorCode::kInvalidArgument)) {
    return status;
  }
  SlottedPage page(guard.mutable_data());
  if (Status status = page.Delete(rid.slot_id)) {
    return status;
  }
  guard.MarkDirty();
  SetCapacity(rid.page_id, page.InsertCapacity());
  return std::nullopt;
}

}  // namespace flatfiledb::paged
