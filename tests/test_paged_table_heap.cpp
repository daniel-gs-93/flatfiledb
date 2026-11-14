#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "flatfiledb/domain/value.h"
#include "flatfiledb/paged/buffer_manager.h"
#include "flatfiledb/paged/byte_codec.h"
#include "flatfiledb/paged/page_store.h"
#include "flatfiledb/paged/slotted_page.h"
#include "flatfiledb/paged/table_heap.h"
#include "test_harness.h"
#include "test_paged_support.h"

// TableHeap over a MemoryPageStore.

namespace {

using flatfiledb::domain::DataType;
using flatfiledb::domain::Row;
using flatfiledb::domain::TableSchema;
using flatfiledb::domain::Value;
using flatfiledb::paged::BufferManager;
using flatfiledb::paged::kInvalidPageId;
using flatfiledb::paged::LatchMode;
using flatfiledb::paged::MemoryPageStore;
using flatfiledb::paged::PageGuard;
using flatfiledb::paged::PageId;
using flatfiledb::paged::ReplacementPolicyKind;
using flatfiledb::paged::Rid;
using flatfiledb::paged::SlottedPage;
using flatfiledb::paged::SlottedPageView;
using flatfiledb::paged::TableHeap;
using flatfiledb::testing::Column;
using flatfiledb::testing::MakeSchema;

constexpr std::uint32_t kTableId = 5;

TableSchema PeopleSchema() {
  return MakeSchema(
      "people", {Column("id", DataType::kInt), Column("name", DataType::kText),
                 Column("score", DataType::kDouble)});
}

Row PersonRow(std::int64_t id, std::size_t name_length = 5) {
  return Row{Value(id),
             Value(std::string(name_length, static_cast<char>('a' + id % 26))),
             Value(static_cast<double>(id) * 0.5)};
}

// A row whose encoded tuple is exactly 100 bytes in PeopleSchema:
// 8 + (2 + 82) + 8.
Row HundredByteRow(std::int64_t id) { return PersonRow(id, 82); }

// A MemoryPageStore, a BufferManager over it, and one TableHeap.
class HeapEnv {
 public:
  explicit HeapEnv(std::size_t frames,
                   ReplacementPolicyKind policy = ReplacementPolicyKind::kLru,
                   TableSchema schema = PeopleSchema())
      : schema_(std::move(schema)) {
    auto created = BufferManager::Create(store_, frames, policy);
    CHECK_OK(created);
    if (!created.ok()) {
      return;
    }
    buffer_ = std::move(created).value();
    auto first = TableHeap::Create(*buffer_, kTableId);
    CHECK_OK(first);
    if (!first.ok()) {
      return;
    }
    auto opened = TableHeap::Open(*buffer_, schema_, kTableId, first.value());
    CHECK_OK(opened);
    if (!opened.ok()) {
      return;
    }
    heap_ = std::make_unique<TableHeap>(std::move(opened).value());
  }

  bool is_ready() const { return heap_ != nullptr; }
  MemoryPageStore& store() { return store_; }
  BufferManager& buffer() { return *buffer_; }
  TableHeap& heap() { return *heap_; }
  const TableSchema& schema() const { return schema_; }

 private:
  MemoryPageStore store_;
  TableSchema schema_;
  std::unique_ptr<BufferManager> buffer_;
  std::unique_ptr<TableHeap> heap_;
};

Rid MustInsert(TableHeap& heap, const Row& row) {
  auto result = heap.Insert(row);
  CHECK_OK(result);
  return result.ok() ? result.value() : Rid{kInvalidPageId, 0};
}

// Every row of the heap with its Rid, in scan order.
std::vector<flatfiledb::paged::ScanItem> ScanAll(const TableHeap& heap) {
  std::vector<flatfiledb::paged::ScanItem> items;
  flatfiledb::paged::HeapScan scan = heap.Scan();
  while (true) {
    auto next = scan.Next();
    CHECK_OK(next);
    if (!next.ok() || !next.value().has_value()) {
      break;
    }
    items.push_back(std::move(*next.value()));
  }
  return items;
}

// The page numbers of the chain, followed through the page headers.
std::vector<PageId> ChainOf(BufferManager& buffer, PageId first_page) {
  std::vector<PageId> pages;
  PageId current = first_page;
  while (current != kInvalidPageId && pages.size() < 1000) {
    auto fetched = buffer.FetchPage(current, LatchMode::kShared);
    CHECK_OK(fetched);
    if (!fetched.ok()) {
      break;
    }
    pages.push_back(current);
    current = SlottedPageView(fetched.value().data()).next_page_id();
  }
  return pages;
}

}  // namespace

TEST(test_paged_heap_insert_get_and_scan) {
  HeapEnv env(8);
  if (!env.is_ready()) {
    return;
  }
  std::vector<Rid> rids;
  std::vector<Row> rows;
  for (std::int64_t id = 0; id < 1000; ++id) {
    rows.push_back(PersonRow(id, static_cast<std::size_t>(id % 20)));
    rids.push_back(MustInsert(env.heap(), rows.back()));
  }
  for (std::size_t i = 0; i < rids.size(); ++i) {
    auto got = env.heap().Get(rids[i]);
    CHECK_OK(got);
    if (got.ok()) {
      CHECK(got.value() == rows[i]);
    }
  }
  const auto items = ScanAll(env.heap());
  CHECK_EQ(items.size(), static_cast<std::size_t>(1000));
  std::set<Rid> seen;
  for (const auto& item : items) {
    seen.insert(item.rid);
  }
  CHECK_EQ(seen.size(), static_cast<std::size_t>(1000));
  for (std::size_t i = 1; i < items.size(); ++i) {
    if (items[i].rid.page_id == items[i - 1].rid.page_id) {
      CHECK(items[i].rid.slot_id > items[i - 1].rid.slot_id);
    }
  }
  CHECK_EQ(env.buffer().PinnedFrameCount(), static_cast<std::size_t>(0));
}

TEST(test_paged_heap_chains_pages_in_insertion_order) {
  HeapEnv env(4);
  if (!env.is_ready()) {
    return;
  }
  const PageId first_page = env.heap().first_page();
  for (std::int64_t id = 0; id < 300; ++id) {
    CHECK_OK(env.heap().Insert(HundredByteRow(id)));
  }
  CHECK_EQ(env.heap().first_page(), first_page);
  const std::vector<PageId> chain = ChainOf(env.buffer(), first_page);
  CHECK_EQ(chain.size(), static_cast<std::size_t>(8));  // ceil(300 / 39)
  CHECK_EQ(std::set<PageId>(chain.begin(), chain.end()).size(),
           static_cast<std::size_t>(8));
  for (PageId page_id : chain) {
    auto fetched = env.buffer().FetchPage(page_id, LatchMode::kShared);
    CHECK_OK(fetched);
    if (!fetched.ok()) {
      continue;
    }
    CHECK_EQ(SlottedPageView(fetched.value().data()).table_id(), kTableId);
  }
  auto tail = env.buffer().FetchPage(chain.back(), LatchMode::kShared);
  CHECK_OK(tail);
  if (tail.ok()) {
    CHECK_EQ(SlottedPageView(tail.value().data()).next_page_id(),
             kInvalidPageId);
  }
  CHECK_EQ(env.heap().tail_page(), chain.back());
}

TEST(test_paged_heap_delete_frees_space_for_reuse) {
  HeapEnv env(8);
  if (!env.is_ready()) {
    return;
  }
  std::vector<Rid> rids;
  for (std::int64_t id = 0; id < 200; ++id) {
    rids.push_back(MustInsert(env.heap(), HundredByteRow(id)));
  }
  std::vector<Rid> deleted;
  for (std::size_t i = 0; i < rids.size(); i += 2) {
    CHECK_OK(env.heap().Delete(rids[i]));
    deleted.push_back(rids[i]);
  }
  CHECK_EQ(ScanAll(env.heap()).size(), static_cast<std::size_t>(100));
  for (const Rid& rid : deleted) {
    CHECK_ERROR(env.heap().Get(rid), kNotFound);
  }
  const flatfiledb::paged::PageId pages_before = env.store().PageCount();
  for (std::int64_t id = 200; id < 300; ++id) {
    CHECK_OK(env.heap().Insert(HundredByteRow(id)));
  }
  CHECK_EQ(env.store().PageCount(), pages_before);
  const auto items = ScanAll(env.heap());
  CHECK_EQ(items.size(), static_cast<std::size_t>(200));
  std::set<std::int64_t> ids;
  for (const auto& item : items) {
    ids.insert(std::get<std::int64_t>(item.row[0]));
  }
  CHECK_EQ(ids.size(), static_cast<std::size_t>(200));
}

TEST(test_paged_heap_update_keeps_rid_or_reports_no_space) {
  HeapEnv env(8);
  if (!env.is_ready()) {
    return;
  }
  // 41 rows of 100 bytes: page 0 holds 39 and is full, page 1 has two.
  std::vector<Rid> rids;
  for (std::int64_t id = 0; id < 41; ++id) {
    rids.push_back(MustInsert(env.heap(), HundredByteRow(id)));
  }
  const Rid on_full_page = rids[0];
  const Rid on_open_page = rids[40];
  CHECK(on_full_page.page_id != on_open_page.page_id);

  // Same length and shorter: the Rid stays and the value changes.
  CHECK_OK(env.heap().Update(on_open_page, PersonRow(1000, 82)));
  auto same = env.heap().Get(on_open_page);
  CHECK_OK(same);
  if (same.ok()) {
    CHECK(same.value() == PersonRow(1000, 82));
  }
  CHECK_OK(env.heap().Update(on_open_page, PersonRow(1001, 10)));
  auto shorter = env.heap().Get(on_open_page);
  CHECK_OK(shorter);
  if (shorter.ok()) {
    CHECK(shorter.value() == PersonRow(1001, 10));
  }
  // Longer and the page has room.
  CHECK_OK(env.heap().Update(on_open_page, PersonRow(1002, 600)));
  auto longer = env.heap().Get(on_open_page);
  CHECK_OK(longer);
  if (longer.ok()) {
    CHECK(longer.value() == PersonRow(1002, 600));
  }
  // Longer and the page is full: nothing changes.
  CHECK_ERROR(env.heap().Update(on_full_page, PersonRow(2000, 300)), kNoSpace);
  auto unchanged = env.heap().Get(on_full_page);
  CHECK_OK(unchanged);
  if (unchanged.ok()) {
    CHECK(unchanged.value() == HundredByteRow(0));
  }
  // A deleted row cannot be updated.
  CHECK_OK(env.heap().Delete(rids[1]));
  CHECK_ERROR(env.heap().Update(rids[1], PersonRow(3000, 82)), kNotFound);
}

TEST(test_paged_heap_rejects_invalid_arguments) {
  HeapEnv env(8);
  if (!env.is_ready()) {
    return;
  }
  const flatfiledb::paged::PageId pages_before = env.store().PageCount();
  // 8 + (2 + 4043) + 8 = 4061 bytes.
  CHECK_ERROR(env.heap().Insert(PersonRow(1, 4043)), kTooLarge);
  CHECK_EQ(env.store().PageCount(), pages_before);
  CHECK_ERROR(env.heap().Insert(Row{Value(std::string("x")),
                                    Value(std::string("y")), Value(1.0)}),
              kInvalidArgument);
  CHECK_EQ(env.store().PageCount(), pages_before);

  // A page of another table.
  auto other_first = TableHeap::Create(env.buffer(), 6);
  CHECK_OK(other_first);
  if (other_first.ok()) {
    CHECK_ERROR(env.heap().Get(Rid{other_first.value(), 0}), kInvalidArgument);
  }
  CHECK_ERROR(env.heap().Get(Rid{9999, 0}), kNotFound);
}

TEST(test_paged_heap_free_space_map_rebuild_matches_incremental) {
  HeapEnv env(8);
  if (!env.is_ready()) {
    return;
  }
  std::mt19937_64 generator(7);
  std::vector<Rid> live;
  for (int step = 0; step < 500; ++step) {
    if (live.empty() || generator() % 100 < 60) {
      live.push_back(MustInsert(
          env.heap(),
          PersonRow(step, static_cast<std::size_t>(generator() % 300))));
    } else {
      const std::size_t index = generator() % live.size();
      CHECK_OK(env.heap().Delete(live[index]));
      live[index] = live.back();
      live.pop_back();
    }
  }
  const auto incremental = env.heap().FreeSpaceSnapshot();
  auto reopened = TableHeap::Open(env.buffer(), env.schema(), kTableId,
                                  env.heap().first_page());
  CHECK_OK(reopened);
  if (!reopened.ok()) {
    return;
  }
  const auto rebuilt = reopened.value().FreeSpaceSnapshot();
  CHECK_EQ(incremental.size(), rebuilt.size());
  if (incremental.size() == rebuilt.size()) {
    for (std::size_t i = 0; i < incremental.size(); ++i) {
      CHECK_EQ(incremental[i].page_id, rebuilt[i].page_id);
      CHECK_EQ(incremental[i].capacity, rebuilt[i].capacity);
    }
  }
  CHECK_EQ(env.heap().tail_page(), reopened.value().tail_page());
}

namespace {

TableSchema TextTableSchema() {
  return MakeSchema(
      "t", {Column("id", DataType::kInt), Column("text", DataType::kText)});
}

Row RandomRow(std::mt19937_64& generator, std::int64_t id) {
  const std::size_t length = generator() % 401;
  return Row{Value(id),
             Value(std::string(length, static_cast<char>('a' + id % 26)))};
}

void RemoveAt(std::vector<Rid>& live, std::size_t index) {
  live[index] = live.back();
  live.pop_back();
}

void CheckScanEqualsModel(const TableHeap& heap,
                          const std::map<Rid, Row>& model) {
  const auto items = ScanAll(heap);
  CHECK_EQ(items.size(), model.size());
  for (const auto& item : items) {
    auto found = model.find(item.rid);
    CHECK(found != model.end());
    if (found != model.end()) {
      CHECK(found->second == item.row);
    }
  }
}

// 20000 random operations against std::map<Rid, Row>: insert 40%, delete 25%,
// update 20%, get 10%, scan 5%. An update that reports kNoSpace is a delete
// followed by an insert, which gives the row a new Rid.
void RunRandomOperations(ReplacementPolicyKind policy, std::uint64_t seed) {
  HeapEnv env(4, policy, TextTableSchema());
  if (!env.is_ready()) {
    return;
  }
  std::mt19937_64 generator(seed);
  std::map<Rid, Row> model;
  std::vector<Rid> live;
  std::int64_t next_id = 0;
  for (int step = 0; step < 20000; ++step) {
    const std::uint64_t choice = generator() % 100;
    if (choice < 40 || live.empty()) {
      const Row row = RandomRow(generator, next_id++);
      auto inserted = env.heap().Insert(row);
      CHECK_OK(inserted);
      if (!inserted.ok()) {
        return;
      }
      CHECK(model.count(inserted.value()) == 0);
      model[inserted.value()] = row;
      live.push_back(inserted.value());
    } else if (choice < 65) {
      const std::size_t index = generator() % live.size();
      CHECK_OK(env.heap().Delete(live[index]));
      model.erase(live[index]);
      RemoveAt(live, index);
    } else if (choice < 85) {
      const std::size_t index = generator() % live.size();
      const Row row = RandomRow(generator, next_id++);
      const flatfiledb::paged::Status updated =
          env.heap().Update(live[index], row);
      if (!updated.has_value()) {
        model[live[index]] = row;
      } else if (updated->code == flatfiledb::paged::PageErrorCode::kNoSpace) {
        CHECK_OK(env.heap().Delete(live[index]));
        model.erase(live[index]);
        auto reinserted = env.heap().Insert(row);
        CHECK_OK(reinserted);
        if (!reinserted.ok()) {
          return;
        }
        model[reinserted.value()] = row;
        live[index] = reinserted.value();
      } else {
        CHECK_OK(updated);
        return;
      }
    } else if (choice < 95) {
      const Rid rid = live[generator() % live.size()];
      auto got = env.heap().Get(rid);
      CHECK_OK(got);
      if (got.ok()) {
        CHECK(got.value() == model[rid]);
      }
    } else {
      CheckScanEqualsModel(env.heap(), model);
    }
  }
  CheckScanEqualsModel(env.heap(), model);
  for (PageId page_id : ChainOf(env.buffer(), env.heap().first_page())) {
    auto fetched = env.buffer().FetchPage(page_id, LatchMode::kShared);
    CHECK_OK(fetched);
    if (fetched.ok()) {
      CHECK_OK(SlottedPageView(fetched.value().data()).CheckStructure());
    }
  }
  CHECK_EQ(env.buffer().PinnedFrameCount(), static_cast<std::size_t>(0));
}

enum class ChainDamage { kNone, kCycle, kForeignTableId, kWrongPageType };

struct OpenOutcome {
  std::string error_name;
  std::string message;
};

// Builds a heap of three pages, damages one page, writes everything out, and
// opens the heap through a new BufferManager over the same store.
OpenOutcome OpenAfterDamage(ChainDamage damage) {
  HeapEnv env(4);
  if (!env.is_ready()) {
    return OpenOutcome{"not ready", ""};
  }
  for (std::int64_t id = 0; id < 100; ++id) {
    CHECK_OK(env.heap().Insert(HundredByteRow(id)));
  }
  const PageId first_page = env.heap().first_page();
  const std::vector<PageId> chain = ChainOf(env.buffer(), first_page);
  CHECK_EQ(chain.size(), static_cast<std::size_t>(3));
  if (chain.size() != 3) {
    return OpenOutcome{"bad chain", ""};
  }
  if (damage != ChainDamage::kNone) {
    const PageId target =
        damage == ChainDamage::kCycle ? chain.back() : chain[1];
    auto fetched = env.buffer().FetchPage(target, LatchMode::kExclusive);
    CHECK_OK(fetched);
    if (!fetched.ok()) {
      return OpenOutcome{"fetch failed", ""};
    }
    PageGuard guard = std::move(fetched).value();
    if (damage == ChainDamage::kCycle) {
      SlottedPage(guard.mutable_data()).set_next_page_id(first_page);
    } else if (damage == ChainDamage::kForeignTableId) {
      flatfiledb::paged::WriteLe32(guard.mutable_data() + 20, 99);
    } else {
      guard.mutable_data()[8] = 1;
    }
    guard.MarkDirty();
  }
  CHECK_OK(env.buffer().FlushAll());
  auto fresh =
      BufferManager::Create(env.store(), 4, ReplacementPolicyKind::kLru);
  CHECK_OK(fresh);
  if (!fresh.ok()) {
    return OpenOutcome{"no buffer", ""};
  }
  std::unique_ptr<BufferManager> buffer = std::move(fresh).value();
  auto opened = TableHeap::Open(*buffer, env.schema(), kTableId, first_page);
  return OpenOutcome{flatfiledb::testing::ErrorName(opened),
                     flatfiledb::testing::MessageOf(opened)};
}

}  // namespace

TEST(test_paged_heap_matches_map_model_under_random_operations) {
  const std::uint64_t seeds[] = {1, 20261111, 987654321};
  for (std::uint64_t seed : seeds) {
    RunRandomOperations(ReplacementPolicyKind::kLru, seed);
    RunRandomOperations(ReplacementPolicyKind::kTwoQ, seed);
  }
}

TEST(test_paged_heap_open_rejects_broken_chains) {
  CHECK_EQ(OpenAfterDamage(ChainDamage::kNone).error_name, "ok");
  const OpenOutcome cycle = OpenAfterDamage(ChainDamage::kCycle);
  CHECK_EQ(cycle.error_name, "kCorrupt");
  CHECK(cycle.message.find("page ") != std::string::npos);
  const OpenOutcome foreign = OpenAfterDamage(ChainDamage::kForeignTableId);
  CHECK_EQ(foreign.error_name, "kCorrupt");
  CHECK(foreign.message.find("page ") != std::string::npos);
  const OpenOutcome wrong_type = OpenAfterDamage(ChainDamage::kWrongPageType);
  CHECK_EQ(wrong_type.error_name, "kCorrupt");
  CHECK(wrong_type.message.find("page ") != std::string::npos);
}

// A scan that failed on the second slot of a page has already decoded the
// first slot. The error ends the scan: every later Next returns no value, and
// never the row that was decoded before the failure.
TEST(test_paged_heap_scan_ends_after_a_page_error) {
  HeapEnv env(4);
  if (!env.is_ready()) {
    return;
  }
  for (std::int64_t id = 0; id < 3; ++id) {
    MustInsert(env.heap(), PersonRow(id));
  }
  CHECK_EQ(ScanAll(env.heap()).size(), static_cast<std::size_t>(3));

  // Slot 1 gets an offset inside the page header: Get reports kCorrupt.
  {
    auto fetched =
        env.buffer().FetchPage(env.heap().first_page(), LatchMode::kExclusive);
    CHECK_OK(fetched);
    if (!fetched.ok()) {
      return;
    }
    PageGuard guard = std::move(fetched).value();
    const std::size_t slot_position =
        flatfiledb::paged::kPageHeaderSize + flatfiledb::paged::kSlotSize;
    flatfiledb::paged::WriteLe16(guard.mutable_data() + slot_position, 8);
    flatfiledb::paged::WriteLe16(guard.mutable_data() + slot_position + 2, 10);
    guard.MarkDirty();
  }

  flatfiledb::paged::HeapScan scan = env.heap().Scan();
  auto first = scan.Next();
  CHECK_ERROR(first, kCorrupt);
  CHECK_MESSAGE_CONTAINS(first, "slot 1");
  for (int call = 0; call < 3; ++call) {
    auto after_error = scan.Next();
    CHECK_OK(after_error);
    if (after_error.ok()) {
      CHECK(!after_error.value().has_value());
    }
  }
  CHECK_EQ(env.buffer().PinnedFrameCount(), static_cast<std::size_t>(0));
}
