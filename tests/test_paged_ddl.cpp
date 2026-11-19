#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "strata/domain/value.h"
#include "strata/paged/buffer_manager.h"
#include "strata/paged/byte_codec.h"
#include "strata/paged/database.h"
#include "strata/paged/page_store.h"
#include "strata/paged/slotted_page.h"
#include "test_harness.h"
#include "test_paged_ddl_support.h"
#include "test_paged_support.h"

// Database::AddColumn, Database::DropTable and the free list at open (design
// document 2.6, 2.10, 2.11, 3.1, 3.4; ADR-0045, ADR-0046).

namespace {

using strata::domain::ColumnDef;
using strata::domain::DataType;
using strata::domain::Row;
using strata::domain::TableSchema;
using strata::domain::Value;
using strata::paged::Database;
using strata::paged::kPageSize;
using strata::paged::MemoryPageStore;
using strata::paged::PageId;
using strata::paged::SlottedPage;
using strata::testing::BodyRow;
using strata::testing::BodySchema;
using strata::testing::ChainOf;
using strata::testing::EveryPageIsAccountedFor;
using strata::testing::MemoryDatabase;
using strata::testing::OpenCopy;
using strata::testing::OpenNewMemoryDatabase;
using strata::testing::RowsOf;
using Operation = strata::paged::MemoryPageStore::Operation;

constexpr const char* kNotReclaimed =
    "; statement was applied but its pages were not reclaimed";

std::vector<PageId> FreePagesOf(Database& database) {
  auto free_pages = database.allocator().FreePages();
  CHECK_OK(free_pages);
  return free_pages.ok() ? free_pages.value() : std::vector<PageId>{};
}

// Creates table `name` (BodySchema) with `row_count` rows of `body_length`.
void FillTable(Database& database, const std::string& name,
               std::int64_t row_count, std::size_t body_length) {
  CHECK_OK(database.CreateTable(BodySchema(name)));
  auto heap = database.Table(name);
  CHECK_OK(heap);
  if (!heap.ok()) {
    return;
  }
  for (std::int64_t id = 1; id <= row_count; ++id) {
    CHECK_OK(heap.value()->Insert(BodyRow(id, body_length)));
  }
}

std::int64_t IdOfRow(const Row& row) { return std::get<std::int64_t>(row[0]); }

void SortById(std::vector<Row>* rows) { std::ranges::sort(*rows, {}, IdOfRow); }

std::vector<std::vector<std::uint8_t>> ImagesOf(const MemoryPageStore& store) {
  std::vector<std::vector<std::uint8_t>> images;
  for (PageId page_id = 0; page_id < store.PageCount(); ++page_id) {
    images.emplace_back(store.PageBytes(page_id),
                        store.PageBytes(page_id) + kPageSize);
  }
  return images;
}

void SealAndStore(MemoryPageStore& store, PageId page_id) {
  SlottedPage::SealChecksum(store.MutablePage(page_id));
}

}  // namespace

TEST(test_paged_database_verifies_the_free_list_at_open) {
  // A closed database with a released chain of 3 pages and a live table.
  std::unique_ptr<MemoryPageStore> base;
  std::vector<PageId> chain;
  PageId live_first = 0;
  {
    MemoryDatabase env = OpenNewMemoryDatabase();
    if (env.database == nullptr) {
      return;
    }
    chain = strata::testing::MakeChain(*env.database, "gone", 3);
    FillTable(*env.database, "live", 4, 2000);
    live_first = *env.database->catalog().FirstPage("live");
    if (chain.size() != 3) {
      return;
    }
    CHECK_OK(env.database->allocator().Release(chain.front()));
    CHECK_OK(env.database->Close());
    base = strata::testing::CopyOfStore(*env.store);
  }
  const PageId page_count = base->PageCount();

  {
    auto healthy = OpenCopy(*base);
    CHECK(healthy.database != nullptr);
    if (healthy.database != nullptr) {
      CHECK_EQ(FreePagesOf(*healthy.database), chain);
    }
  }

  struct Damage {
    const char* name;
    PageId head;       // free_list_head to write, or kInvalidPageId to keep
    PageId next_page;  // next_page_id of chain[1] to write, or kInvalidPageId
    const char* message;
  };
  const Damage damages[] = {
      {"empty", 0, strata::paged::kInvalidPageId, nullptr},
      {"past the end", page_count + 5, strata::paged::kInvalidPageId,
       "past the end of the file"},
      {"the file header", 0xFFFFFFFE, strata::paged::kInvalidPageId, nullptr},
      {"a live table page", live_first, strata::paged::kInvalidPageId,
       "page_type 2 instead of 3"},
      {"a loop", strata::paged::kInvalidPageId, chain[1], "visits page"},
      {"a heap page in the middle", strata::paged::kInvalidPageId, live_first,
       "page_type 2 instead of 3"},
  };
  for (const Damage& damage : damages) {
    auto copy = strata::testing::CopyOfStore(*base);
    if (damage.head != strata::paged::kInvalidPageId &&
        damage.head != 0xFFFFFFFE) {
      strata::paged::WriteLe32(
          copy->MutablePage(0) + strata::paged::layout::kFreeListHead,
          damage.head);
      SealAndStore(*copy, 0);
    }
    if (std::string(damage.name) == "the file header") {
      // free_list_head 0 is "empty", so the header is reached through a link.
      strata::paged::WriteLe32(
          copy->MutablePage(chain[2]) + strata::paged::layout::kNextPageId, 0);
      SealAndStore(*copy, chain[2]);
    }
    if (damage.next_page != strata::paged::kInvalidPageId) {
      strata::paged::WriteLe32(
          copy->MutablePage(chain[1]) + strata::paged::layout::kNextPageId,
          damage.next_page);
      SealAndStore(*copy, chain[1]);
    }
    auto opened = OpenCopy(*copy);
    if (std::string(damage.name) == "empty") {
      CHECK(opened.database != nullptr);
      if (opened.database != nullptr) {
        CHECK(FreePagesOf(*opened.database).empty());
      }
      continue;
    }
    CHECK(opened.database == nullptr);
    CHECK(opened.error.code == strata::paged::PageErrorCode::kCorrupt);
    if (damage.message != nullptr) {
      CHECK(opened.error.message.find(damage.message) != std::string::npos);
    } else {
      CHECK(opened.error.message.find("page_type 1 instead of 3") !=
            std::string::npos);
    }
  }
}

TEST(test_paged_ddl_add_column_rewrites_rows_and_survives_reopen) {
  MemoryDatabase env = OpenNewMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  CHECK_OK(database.CreateTable(strata::testing::MakeSchema(
      "t", {strata::testing::Column("id", DataType::kInt),
            strata::testing::Column("code", DataType::kText),
            strata::testing::Column("body", DataType::kText)})));
  {
    auto heap = database.Table("t");
    CHECK_OK(heap);
    if (!heap.ok()) {
      return;
    }
    for (std::int64_t id = 1; id <= 300; ++id) {
      CHECK_OK(heap.value()->Insert(Row{
          Value(id), Value("code-" + std::to_string(id)),
          Value(std::string(static_cast<std::size_t>(id % 90) + 100, 'x'))}));
    }
  }
  std::vector<Row> expected = RowsOf(database, "t");
  SortById(&expected);
  CHECK_EQ(expected.size(), std::size_t{300});
  const auto table_id = database.catalog().TableId("t");

  const ColumnDef columns[] = {
      {"a", DataType::kInt, Value(std::int64_t{7})},
      {"b", DataType::kDouble, Value(1.5)},
      {"c", DataType::kText, Value(std::string("x"))},
  };
  bool is_first = true;
  for (const ColumnDef& column : columns) {
    const PageId old_first = *database.catalog().FirstPage("T");
    const std::vector<PageId> old_chain = ChainOf(database.buffer(), old_first);
    auto altered = database.AddColumn(is_first ? "t" : "T", column);
    CHECK_OK(altered);
    if (!altered.ok()) {
      return;
    }
    for (Row& row : expected) {
      row.push_back(*column.default_value);
    }
    std::vector<Row> actual = RowsOf(database, "t");
    SortById(&actual);
    CHECK(actual == expected);
    const auto schema = database.catalog().FindTable("t");
    CHECK(schema.has_value());
    if (schema.has_value()) {
      CHECK(schema->columns.back().name == column.name);
      CHECK(schema->primary_key_index == 0);
    }
    CHECK(database.catalog().TableId("t") == table_id);
    CHECK(EveryPageIsAccountedFor(database));
    if (is_first) {
      CHECK_EQ(FreePagesOf(database), old_chain);
    }
    is_first = false;
  }

  CHECK_OK(database.Close());
  auto reopened = OpenCopy(*env.store);
  CHECK(reopened.database != nullptr);
  if (reopened.database == nullptr) {
    return;
  }
  const auto schema = reopened.database->catalog().FindTable("t");
  CHECK(schema.has_value());
  if (schema.has_value()) {
    CHECK_EQ(schema->columns.size(), std::size_t{6});
  }
  std::vector<Row> after = RowsOf(*reopened.database, "t");
  SortById(&after);
  CHECK(after == expected);
  CHECK(EveryPageIsAccountedFor(*reopened.database));
}

TEST(test_paged_ddl_add_column_boundaries) {
  MemoryDatabase env = OpenNewMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  // A row of 8 + (2 + 4042) = 4052 bytes fits one more INT (8 bytes); a row of
  // 4053 bytes does not.
  CHECK_OK(database.CreateTable(BodySchema("fits")));
  CHECK_OK(database.CreateTable(BodySchema("too_long")));
  CHECK_OK(database.CreateTable(BodySchema("plain")));
  CHECK_OK(database.Table("fits").value()->Insert(BodyRow(1, 4042)));
  CHECK_OK(database.Table("too_long").value()->Insert(BodyRow(1, 4043)));
  CHECK_OK(database.Table("plain").value()->Insert(BodyRow(1, 10)));
  CHECK_OK(database.buffer().FlushAll());

  const ColumnDef int_column{"extra", DataType::kInt, Value(std::int64_t{0})};
  CHECK_OK(database.AddColumn("fits", int_column));

  CHECK_OK(database.buffer().FlushAll());
  const auto before = ImagesOf(*env.store);
  const std::vector<PageId> free_before = FreePagesOf(database);
  const auto too_large = database.AddColumn("too_long", int_column);
  CHECK_ERROR(too_large, kTooLarge);
  const auto long_default = database.AddColumn(
      "plain",
      ColumnDef{"note", DataType::kText, Value(std::string(4100, 'x'))});
  CHECK_ERROR(long_default, kInvalidArgument);
  CHECK_MESSAGE_CONTAINS(long_default, "column note");
  CHECK_OK(database.buffer().FlushAll());
  CHECK(ImagesOf(*env.store) == before);
  CHECK_EQ(FreePagesOf(database), free_before);
  CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
  CHECK_EQ(RowsOf(database, "too_long").size(), std::size_t{1});
  CHECK_EQ(database.catalog().FindTable("too_long")->columns.size(),
           std::size_t{2});
}

TEST(test_paged_ddl_add_column_fails_cleanly) {
  std::unique_ptr<MemoryPageStore> base;
  {
    MemoryDatabase env = OpenNewMemoryDatabase();
    if (env.database == nullptr) {
      return;
    }
    FillTable(*env.database, "spare", 12, 2000);  // about 6 pages, then freed
    FillTable(*env.database, "t", 40, 2000);      // about 20 pages
    CHECK_OK(env.database->DropTable("spare"));
    CHECK_OK(env.database->Close());
    base = strata::testing::CopyOfStore(*env.store);
  }
  const ColumnDef column{"c", DataType::kInt, Value(std::int64_t{7})};
  const Operation operations[] = {Operation::kRead, Operation::kWrite,
                                  Operation::kAllocate};
  int not_reclaimed_cases = 0;
  int rolled_back_cases = 0;
  for (const Operation operation : operations) {
    bool has_succeeded = false;
    int failures_seen = 0;
    for (int nth_call = 1; nth_call < 600 && !has_succeeded; ++nth_call) {
      auto opened = OpenCopy(*base, 4);
      CHECK(opened.database != nullptr);
      if (opened.database == nullptr) {
        return;
      }
      Database& database = *opened.database;
      const std::vector<Row> old_rows = RowsOf(database, "t");
      opened.store->InjectFailure(operation, nth_call);
      const auto result = database.AddColumn("t", column);
      opened.store->ClearInjectedFailures();
      CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
      const bool is_not_reclaimed =
          !result.ok() && result.error().message.ends_with(kNotReclaimed);
      if (result.ok()) {
        has_succeeded = true;
      } else {
        CHECK_ERROR(result, kIo);
        ++failures_seen;
      }
      const bool is_new = result.ok() || is_not_reclaimed;
      not_reclaimed_cases += is_not_reclaimed ? 1 : 0;
      rolled_back_cases += (!result.ok() && !is_not_reclaimed) ? 1 : 0;
      const auto schema = database.catalog().FindTable("t");
      CHECK(schema.has_value());
      if (schema.has_value()) {
        CHECK_EQ(schema->columns.size(),
                 is_new ? std::size_t{3} : std::size_t{2});
      }
      std::vector<Row> expected = old_rows;
      if (is_new) {
        for (Row& row : expected) {
          row.push_back(*column.default_value);
        }
      }
      std::vector<Row> actual = RowsOf(database, "t");
      SortById(&actual);
      SortById(&expected);
      CHECK(actual == expected);

      CHECK_OK(database.Close());
      auto reopened = OpenCopy(*opened.store, 16);
      CHECK(reopened.database != nullptr);
      if (reopened.database != nullptr) {
        std::vector<Row> after = RowsOf(*reopened.database, "t");
        SortById(&after);
        CHECK(after == expected);
      }
    }
    CHECK(has_succeeded);
    CHECK(failures_seen > 0);
  }
  // Both kinds of failure were reached: before the switch and after it.
  CHECK(not_reclaimed_cases > 0);
  CHECK(rolled_back_cases > 0);
}

TEST(test_paged_ddl_drop_table_frees_pages_and_reuses_them) {
  MemoryDatabase env = OpenNewMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  FillTable(database, "a", 20, 2000);
  FillTable(database, "b", 20, 2000);
  FillTable(database, "c", 20, 2000);
  const std::vector<PageId> b_chain =
      ChainOf(database.buffer(), *database.catalog().FirstPage("b"));
  const std::vector<Row> a_rows = RowsOf(database, "a");
  const std::vector<Row> c_rows = RowsOf(database, "c");
  const PageId page_count = database.buffer().PageCount();
  const auto b_id = database.catalog().TableId("b");

  CHECK_OK(database.DropTable("B"));
  CHECK_ERROR(database.Table("b"), kNotFound);
  CHECK_EQ(database.catalog().TableNames(),
           (std::vector<std::string>{"a", "c"}));
  CHECK(RowsOf(database, "a") == a_rows);
  CHECK(RowsOf(database, "c") == c_rows);
  CHECK_EQ(FreePagesOf(database), b_chain);
  CHECK(EveryPageIsAccountedFor(database));

  CHECK_OK(database.Close());
  auto reopened = OpenCopy(*env.store);
  CHECK(reopened.database != nullptr);
  if (reopened.database == nullptr) {
    return;
  }
  Database& second = *reopened.database;
  CHECK_ERROR(second.Table("b"), kNotFound);
  CHECK_EQ(FreePagesOf(second), b_chain);

  FillTable(second, "d", 20, 2000);
  CHECK(second.catalog().TableId("d").has_value());
  if (second.catalog().TableId("d").has_value() && b_id.has_value()) {
    CHECK(*second.catalog().TableId("d") > *second.catalog().TableId("c"));
    CHECK(*second.catalog().TableId("d") != *b_id);
  }
  CHECK_EQ(second.buffer().PageCount(), page_count);
  CHECK(EveryPageIsAccountedFor(second));
  CHECK_EQ(RowsOf(second, "d").size(), std::size_t{20});
}

TEST(test_paged_ddl_drop_table_fails_cleanly) {
  std::unique_ptr<MemoryPageStore> base;
  {
    MemoryDatabase env = OpenNewMemoryDatabase();
    if (env.database == nullptr) {
      return;
    }
    FillTable(*env.database, "keep", 5, 2000);
    FillTable(*env.database, "spare", 12, 2000);  // about 6 pages, then freed
    FillTable(*env.database, "t", 40, 2000);      // about 20 pages
    CHECK_OK(env.database->DropTable("spare"));
    CHECK_OK(env.database->Close());
    base = strata::testing::CopyOfStore(*env.store);
  }
  const Operation operations[] = {Operation::kRead, Operation::kWrite,
                                  Operation::kAllocate};
  int not_reclaimed_cases = 0;
  int rolled_back_cases = 0;
  for (const Operation operation : operations) {
    bool has_succeeded = false;
    int failures_seen = 0;
    for (int nth_call = 1; nth_call < 600 && !has_succeeded; ++nth_call) {
      auto opened = OpenCopy(*base, 4);
      CHECK(opened.database != nullptr);
      if (opened.database == nullptr) {
        return;
      }
      Database& database = *opened.database;
      const std::vector<Row> old_rows = RowsOf(database, "t");
      opened.store->InjectFailure(operation, nth_call);
      const auto status = database.DropTable("t");
      opened.store->ClearInjectedFailures();
      CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
      const bool is_not_reclaimed =
          status.has_value() && status->message.ends_with(kNotReclaimed);
      if (!status.has_value()) {
        has_succeeded = true;
      } else {
        CHECK_ERROR(status, kIo);
        ++failures_seen;
      }
      const bool is_gone = !status.has_value() || is_not_reclaimed;
      not_reclaimed_cases += is_not_reclaimed ? 1 : 0;
      rolled_back_cases += (status.has_value() && !is_not_reclaimed) ? 1 : 0;
      CHECK_EQ(database.catalog().FindTable("t").has_value(), !is_gone);
      if (!is_gone) {
        std::vector<Row> actual = RowsOf(database, "t");
        std::vector<Row> expected = old_rows;
        SortById(&actual);
        SortById(&expected);
        CHECK(actual == expected);
      }
      CHECK_EQ(RowsOf(database, "keep").size(), std::size_t{5});
      CHECK_OK(database.Close());
      auto reopened = OpenCopy(*opened.store, 16);
      CHECK(reopened.database != nullptr);
      if (reopened.database != nullptr) {
        CHECK_EQ(reopened.database->catalog().FindTable("t").has_value(),
                 !is_gone);
        CHECK_EQ(RowsOf(*reopened.database, "keep").size(), std::size_t{5});
      }
    }
    CHECK(has_succeeded);
    // DROP TABLE allocates no page, so an allocation failure never fires.
    CHECK(failures_seen > 0 || operation == Operation::kAllocate);
  }
  CHECK(not_reclaimed_cases > 0);
  CHECK(rolled_back_cases > 0);
}

TEST(test_paged_ddl_double_faults_report_the_undo_failure) {
  std::unique_ptr<MemoryPageStore> base;
  {
    MemoryDatabase env = OpenNewMemoryDatabase();
    if (env.database == nullptr) {
      return;
    }
    FillTable(*env.database, "t", 40, 2000);  // about 20 pages
    // A table of 300 columns: its sys_columns rows span several pages, so a
    // DROP TABLE touches more pages than the pool of 4 frames holds.
    TableSchema wide = BodySchema("wide");
    wide.columns.clear();
    for (int index = 0; index < 300; ++index) {
      wide.columns.push_back(
          strata::testing::Column("c" + std::to_string(index), DataType::kInt));
    }
    CHECK_OK(env.database->CreateTable(wide));
    CHECK_OK(env.database->Close());
    base = strata::testing::CopyOfStore(*env.store);
  }
  const ColumnDef column{"c", DataType::kInt, Value(std::int64_t{7})};
  int undo_failures = 0;
  int release_failures = 0;
  for (int read_call = 1; read_call < 160; ++read_call) {
    for (int write_call = 1; write_call <= 8; ++write_call) {
      for (int statement = 0; statement < 2; ++statement) {
        auto opened = OpenCopy(*base, 4);
        CHECK(opened.database != nullptr);
        if (opened.database == nullptr) {
          return;
        }
        Database& database = *opened.database;
        opened.store->InjectFailure(Operation::kRead, read_call);
        opened.store->InjectFailure(Operation::kWrite, write_call);
        std::string message;
        if (statement == 0) {
          const auto result = database.AddColumn("t", column);
          message = strata::testing::MessageOf(result);
        } else {
          message = strata::testing::MessageOf(database.DropTable("wide"));
        }
        opened.store->ClearInjectedFailures();
        // Whatever happened, no pin is left and the call returned.
        CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
        undo_failures += message.find("; undoing the rows also failed:") !=
                         std::string::npos;
        release_failures +=
            message.find("; releasing the new pages also failed:") !=
            std::string::npos;
      }
    }
  }
  // The two undo branches were reached at least once.
  CHECK(undo_failures > 0);
  CHECK(release_failures > 0);
}

TEST(test_paged_ddl_cycles_keep_the_file_bounded) {
  MemoryDatabase env = OpenNewMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  PageId after_first_cycle = 0;
  for (int cycle = 0; cycle < 40; ++cycle) {
    FillTable(database, "cycle", 100, 1000);
    CHECK_OK(database.AddColumn(
        "cycle", ColumnDef{"extra", DataType::kInt, Value(std::int64_t{0})}));
    CHECK_OK(database.DropTable("cycle"));
    if (cycle == 0) {
      after_first_cycle = database.buffer().PageCount();
    }
  }
  CHECK(database.buffer().PageCount() <= after_first_cycle + 2);
  CHECK(EveryPageIsAccountedFor(database));
}

TEST(test_paged_ddl_refuses_a_corrupt_table) {
  std::unique_ptr<MemoryPageStore> base;
  std::vector<PageId> damaged_chain;
  {
    MemoryDatabase env = OpenNewMemoryDatabase();
    if (env.database == nullptr) {
      return;
    }
    FillTable(*env.database, "damaged", 12, 2000);
    FillTable(*env.database, "healthy", 6, 2000);
    damaged_chain = ChainOf(env.database->buffer(),
                            *env.database->catalog().FirstPage("damaged"));
    CHECK_OK(env.database->Close());
    base = strata::testing::CopyOfStore(*env.store);
  }
  if (damaged_chain.size() < 3) {
    return;
  }
  // One flipped byte in a middle page, without a new checksum.
  base->MutablePage(damaged_chain[1])[kPageSize - 1] ^= 0xFF;

  auto opened = OpenCopy(*base);
  CHECK(opened.database != nullptr);
  if (opened.database == nullptr) {
    return;
  }
  Database& database = *opened.database;
  const std::vector<PageId> free_before = FreePagesOf(database);
  CHECK_ERROR(database.AddColumn("damaged", ColumnDef{"c", DataType::kInt,
                                                      Value(std::int64_t{0})}),
              kCorrupt);
  CHECK_ERROR(database.DropTable("damaged"), kCorrupt);
  CHECK(database.catalog().FindTable("damaged").has_value());
  CHECK_EQ(database.catalog().FindTable("damaged")->columns.size(),
           std::size_t{2});
  CHECK_EQ(FreePagesOf(database), free_before);

  CHECK_OK(database.AddColumn(
      "healthy", ColumnDef{"c", DataType::kInt, Value(std::int64_t{0})}));
  CHECK_OK(database.DropTable("healthy"));
}

TEST(test_paged_ddl_runs_with_four_frames) {
  MemoryDatabase env = OpenNewMemoryDatabase(4);
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  FillTable(database, "t", 40, 2000);  // about 20 pages
  CHECK_OK(database.AddColumn(
      "t", ColumnDef{"c", DataType::kInt, Value(std::int64_t{3})}));
  CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
  CHECK_EQ(RowsOf(database, "t").size(), std::size_t{40});
  CHECK_OK(database.DropTable("t"));
  CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});

  const PageId page_count = database.buffer().PageCount();
  FillTable(database, "u", 40, 2000);
  CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
  CHECK_EQ(RowsOf(database, "u").size(), std::size_t{40});
  CHECK_EQ(database.buffer().PageCount(), page_count);
}
