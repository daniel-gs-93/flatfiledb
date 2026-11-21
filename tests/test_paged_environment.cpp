#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "strata/domain/value.h"
#include "strata/paged/database.h"
#include "strata/paged/slotted_page.h"
#include "test_harness.h"
#include "test_paged_ddl_support.h"
#include "test_paged_support.h"
#include "test_scale_data_support.h"
#include "test_support.h"

// The paged layer on the generated volume data (test_scale_data_support.h)
// with a buffer pool smaller than the data.

namespace {

using strata::domain::Row;
using strata::domain::Value;
using strata::paged::Database;
using strata::paged::DatabaseOptions;
using strata::paged::LatchMode;
using strata::paged::ReplacementPolicyKind;
using strata::paged::SlottedPageView;
using strata::testing::ScaleTable;
using strata::testing::ScaleTables;
using strata::testing::TempDir;

constexpr std::size_t kPoolFrames = 64;  // 256 KiB, below the data size

bool RowIdLess(const Row& left, const Row& right) {
  return std::get<std::int64_t>(left[0]) < std::get<std::int64_t>(right[0]);
}

std::unique_ptr<Database> OpenDatabase(const std::filesystem::path& directory,
                                       ReplacementPolicyKind policy) {
  DatabaseOptions options;
  options.pool_frames = kPoolFrames;
  options.policy = policy;
  auto opened = Database::Open(directory, options);
  CHECK_OK(opened);
  if (!opened.ok()) {
    return nullptr;
  }
  return std::make_unique<Database>(std::move(opened).value());
}

// Creates the three tables, inserts every row, and closes the database.
void BuildDatabase(const std::filesystem::path& directory,
                   const std::vector<ScaleTable>& tables) {
  auto database = OpenDatabase(directory, ReplacementPolicyKind::kLru);
  if (database == nullptr) {
    return;
  }
  for (const ScaleTable& table : tables) {
    CHECK_OK(database->CreateTable(table.schema));
    auto heap = database->Table(table.schema.name);
    CHECK_OK(heap);
    if (!heap.ok()) {
      return;
    }
    for (const Row& row : table.rows) {
      CHECK_OK(heap.value()->Insert(row));
    }
  }
  CHECK_OK(database->Close());
}

std::vector<Row> ScanSorted(Database& database, const std::string& name) {
  std::vector<Row> rows;
  auto heap = database.Table(name);
  CHECK_OK(heap);
  if (!heap.ok()) {
    return rows;
  }
  auto scan = heap.value()->Scan();
  while (true) {
    auto next = scan.Next();
    CHECK_OK(next);
    if (!next.ok() || !next.value().has_value()) {
      break;
    }
    rows.push_back(std::move(next.value()->row));
  }
  std::ranges::sort(rows, RowIdLess);
  return rows;
}

}  // namespace

TEST(test_paged_environment_loads_data_with_small_pool) {
  const std::vector<ScaleTable>& tables = ScaleTables();
  for (const ScaleTable& table : tables) {
    CHECK_EQ(table.rows.size(), static_cast<std::size_t>(10000));
  }
  TempDir directory;
  auto database = OpenDatabase(directory.path(), ReplacementPolicyKind::kLru);
  if (database == nullptr) {
    return;
  }
  for (const ScaleTable& table : tables) {
    CHECK_OK(database->CreateTable(table.schema));
    auto heap = database->Table(table.schema.name);
    CHECK_OK(heap);
    if (!heap.ok()) {
      return;
    }
    for (const Row& row : table.rows) {
      CHECK_OK(heap.value()->Insert(row));
    }
  }
  for (const ScaleTable& table : tables) {
    const std::vector<Row> scanned = ScanSorted(*database, table.schema.name);
    CHECK_EQ(scanned.size(), table.rows.size());
    CHECK(scanned == table.rows);
  }
  CHECK(database->buffer().Stats().evictions > 0);
  CHECK_OK(database->Close());
}

TEST(test_paged_environment_reopen_scans_match_the_rows) {
  const std::vector<ScaleTable>& tables = ScaleTables();
  TempDir directory;
  BuildDatabase(directory.path(), tables);
  const ReplacementPolicyKind policies[] = {ReplacementPolicyKind::kLru,
                                            ReplacementPolicyKind::kTwoQ};
  for (ReplacementPolicyKind policy : policies) {
    auto database = OpenDatabase(directory.path(), policy);
    if (database == nullptr) {
      return;
    }
    std::size_t total_pages = 0;
    for (const ScaleTable& table : tables) {
      auto heap = database->Table(table.schema.name);
      CHECK_OK(heap);
      if (!heap.ok()) {
        return;
      }
      total_pages += heap.value()->FreeSpaceSnapshot().size();
    }
    database->buffer().ResetStats();
    for (const ScaleTable& table : tables) {
      CHECK(ScanSorted(*database, table.schema.name) == table.rows);
    }
    const auto stats = database->buffer().Stats();
    CHECK_EQ(stats.page_reads, stats.misses);
    CHECK_EQ(stats.hits + stats.misses,
             static_cast<std::uint64_t>(total_pages));
  }
}

TEST(test_paged_environment_reuses_space_after_deletes) {
  const std::vector<ScaleTable>& tables = ScaleTables();
  TempDir directory;
  BuildDatabase(directory.path(), tables);
  auto database = OpenDatabase(directory.path(), ReplacementPolicyKind::kLru);
  if (database == nullptr) {
    return;
  }
  const ScaleTable& parcels = tables[1];
  auto heap = database->Table("parcels");
  CHECK_OK(heap);
  if (!heap.ok()) {
    return;
  }
  const strata::paged::PageId pages_before = database->buffer().PageCount();

  std::vector<strata::paged::ScanItem> items;
  auto scan = heap.value()->Scan();
  while (true) {
    auto next = scan.Next();
    CHECK_OK(next);
    if (!next.ok() || !next.value().has_value()) {
      break;
    }
    items.push_back(std::move(*next.value()));
  }
  CHECK_EQ(items.size(), static_cast<std::size_t>(10000));
  std::vector<Row> removed;
  for (std::size_t i = 0; i < items.size(); i += 2) {
    CHECK_OK(heap.value()->Delete(items[i].rid));
    removed.push_back(items[i].row);
  }
  CHECK_EQ(removed.size(), static_cast<std::size_t>(5000));
  for (const Row& row : removed) {
    CHECK_OK(heap.value()->Insert(row));
  }

  CHECK(database->buffer().PageCount() <= pages_before + 2);
  CHECK(ScanSorted(*database, "parcels") == parcels.rows);
  for (const auto& entry : heap.value()->FreeSpaceSnapshot()) {
    auto fetched =
        database->buffer().FetchPage(entry.page_id, LatchMode::kShared);
    CHECK_OK(fetched);
    if (fetched.ok()) {
      CHECK_OK(SlottedPageView(fetched.value().data()).CheckStructure());
    }
  }
}

TEST(test_paged_environment_alter_and_drop_reuse_pages) {
  const std::vector<ScaleTable>& tables = ScaleTables();
  TempDir directory;
  BuildDatabase(directory.path(), tables);
  auto database = OpenDatabase(directory.path(), ReplacementPolicyKind::kLru);
  if (database == nullptr) {
    return;
  }

  // ADD COLUMN on the 10000-row parcels table: every row gets the default.
  CHECK_OK(database->AddColumn(
      "parcels",
      strata::domain::ColumnDef{"flag", strata::domain::DataType::kInt,
                                Value(std::int64_t{0})}));
  std::vector<Row> parcels_with_flag = tables[1].rows;
  for (Row& row : parcels_with_flag) {
    row.push_back(Value(std::int64_t{0}));
  }
  CHECK(ScanSorted(*database, "parcels") == parcels_with_flag);
  const strata::paged::PageId pages_after_alter =
      database->buffer().PageCount();

  // DROP TABLE and a new load of the same data reuse the freed pages.
  CHECK_OK(database->DropTable("scans"));
  CHECK_OK(database->CreateTable(tables[2].schema));
  auto heap = database->Table("scans");
  CHECK_OK(heap);
  if (!heap.ok()) {
    return;
  }
  for (const Row& row : tables[2].rows) {
    CHECK_OK(heap.value()->Insert(row));
  }
  CHECK(database->buffer().PageCount() <= pages_after_alter + 2);
  CHECK(strata::testing::EveryPageIsAccountedFor(*database));
  CHECK(ScanSorted(*database, "depots") == tables[0].rows);
  CHECK(ScanSorted(*database, "scans") == tables[2].rows);

  CHECK_OK(database->Close());
  database = OpenDatabase(directory.path(), ReplacementPolicyKind::kTwoQ);
  if (database == nullptr) {
    return;
  }
  CHECK(ScanSorted(*database, "parcels") == parcels_with_flag);
  CHECK(ScanSorted(*database, "scans") == tables[2].rows);
  CHECK(strata::testing::EveryPageIsAccountedFor(*database));
  for (const std::string& name : database->catalog().TableNames()) {
    auto reopened = database->Table(name);
    CHECK_OK(reopened);
    if (!reopened.ok()) {
      return;
    }
    for (const auto& entry : reopened.value()->FreeSpaceSnapshot()) {
      auto fetched =
          database->buffer().FetchPage(entry.page_id, LatchMode::kShared);
      CHECK_OK(fetched);
      if (fetched.ok()) {
        CHECK_OK(SlottedPageView(fetched.value().data()).CheckStructure());
      }
    }
  }
}
