#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "strata/csv_file_source.h"
#include "strata/domain/value.h"
#include "strata/paged/database.h"
#include "strata/paged/slotted_page.h"
#include "test_harness.h"
#include "test_paged_ddl_support.h"
#include "test_paged_support.h"
#include "test_support.h"

#ifndef STRATA_SOURCE_DIR
#error "STRATA_SOURCE_DIR must be defined by the build"
#endif

// The paged layer on the real data/ directory with a buffer pool smaller than
// the data. Each test is skipped when data/ is missing.

namespace {

using strata::domain::DataType;
using strata::domain::Row;
using strata::domain::TableSchema;
using strata::domain::Value;
using strata::paged::Database;
using strata::paged::DatabaseOptions;
using strata::paged::LatchMode;
using strata::paged::ReplacementPolicyKind;
using strata::paged::Rid;
using strata::paged::SlottedPageView;
using strata::testing::Column;
using strata::testing::MakeSchema;
using strata::testing::ReadFile;
using strata::testing::TempDir;

constexpr std::size_t kPoolFrames = 64;  // 256 KiB, below the data size

std::filesystem::path DataDirectory() {
  return std::filesystem::path(STRATA_SOURCE_DIR) / "data";
}

bool HasData() {
  std::error_code error;
  return std::filesystem::exists(DataDirectory() / "users.csv", error) &&
         std::filesystem::exists(DataDirectory() / "posts.csv", error) &&
         std::filesystem::exists(DataDirectory() / "engagements.csv", error);
}

struct CsvTable {
  TableSchema schema;
  std::vector<Row> rows;  // sorted by id
};

TableSchema UsersSchema() {
  return MakeSchema("users", {Column("id", DataType::kInt),
                              Column("username", DataType::kText),
                              Column("location", DataType::kText)});
}
TableSchema PostsSchema() {
  return MakeSchema(
      "posts",
      {Column("id", DataType::kInt), Column("content", DataType::kText),
       Column("username", DataType::kText), Column("views", DataType::kInt)});
}
TableSchema EngagementsSchema() {
  return MakeSchema(
      "engagements",
      {Column("id", DataType::kInt), Column("postid", DataType::kInt),
       Column("username", DataType::kText), Column("type", DataType::kText),
       Column("comment", DataType::kText),
       Column("timestamp", DataType::kInt)});
}

// The row, or no value when the field count differs from the schema or an
// INT field is not a whole decimal number: the rows flatfile rejects.
std::optional<Row> ConvertFields(const std::vector<std::string>& fields,
                                 const TableSchema& schema) {
  if (fields.size() != schema.columns.size()) {
    return std::nullopt;
  }
  Row row;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (schema.columns[i].type == DataType::kText) {
      row.emplace_back(fields[i]);
      continue;
    }
    std::int64_t number = 0;
    const char* const end = fields[i].data() + fields[i].size();
    const auto parsed = std::from_chars(fields[i].data(), end, number);
    if (fields[i].empty() || parsed.ec != std::errc() || parsed.ptr != end) {
      return std::nullopt;
    }
    row.emplace_back(number);
  }
  return row;
}

bool RowIdLess(const Row& left, const Row& right) {
  return std::get<std::int64_t>(left[0]) < std::get<std::int64_t>(right[0]);
}

CsvTable LoadCsv(const TableSchema& schema) {
  CsvTable table;
  table.schema = schema;
  strata::infrastructure::FileCsvSource source;
  auto read = source.Read((DataDirectory() / (schema.name + ".csv")).string());
  CHECK(std::holds_alternative<std::vector<strata::domain::CsvLine>>(read));
  if (!std::holds_alternative<std::vector<strata::domain::CsvLine>>(read)) {
    return table;
  }
  const auto& lines = std::get<std::vector<strata::domain::CsvLine>>(read);
  for (std::size_t i = 1; i < lines.size(); ++i) {  // line 0 is the header
    if (!lines[i].fields.has_value()) {
      continue;
    }
    std::optional<Row> row = ConvertFields(*lines[i].fields, schema);
    if (row.has_value()) {
      table.rows.push_back(std::move(*row));
    }
  }
  std::ranges::sort(table.rows, RowIdLess);
  return table;
}

std::vector<CsvTable> LoadAllCsv() {
  std::vector<CsvTable> tables;
  tables.push_back(LoadCsv(UsersSchema()));
  tables.push_back(LoadCsv(PostsSchema()));
  tables.push_back(LoadCsv(EngagementsSchema()));
  return tables;
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
                   const std::vector<CsvTable>& tables) {
  auto database = OpenDatabase(directory, ReplacementPolicyKind::kLru);
  if (database == nullptr) {
    return;
  }
  for (const CsvTable& table : tables) {
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

std::string DataBytes() {
  return ReadFile(DataDirectory() / "users.csv") +
         ReadFile(DataDirectory() / "posts.csv") +
         ReadFile(DataDirectory() / "engagements.csv");
}

}  // namespace

TEST(test_paged_environment_loads_data_with_small_pool) {
  if (!HasData()) {
    SKIP("data/ is missing");
  }
  const std::string data_before = DataBytes();
  const std::vector<CsvTable> tables = LoadAllCsv();
  for (const CsvTable& table : tables) {
    CHECK_EQ(table.rows.size(), static_cast<std::size_t>(10000));
  }
  TempDir directory;
  auto database = OpenDatabase(directory.path(), ReplacementPolicyKind::kLru);
  if (database == nullptr) {
    return;
  }
  for (const CsvTable& table : tables) {
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
  for (const CsvTable& table : tables) {
    const std::vector<Row> scanned = ScanSorted(*database, table.schema.name);
    CHECK_EQ(scanned.size(), table.rows.size());
    CHECK(scanned == table.rows);
  }
  CHECK(database->buffer().Stats().evictions > 0);
  CHECK_OK(database->Close());
  CHECK(DataBytes() == data_before);
}

TEST(test_paged_environment_reopen_scans_match_csv) {
  if (!HasData()) {
    SKIP("data/ is missing");
  }
  const std::vector<CsvTable> tables = LoadAllCsv();
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
    for (const CsvTable& table : tables) {
      auto heap = database->Table(table.schema.name);
      CHECK_OK(heap);
      if (!heap.ok()) {
        return;
      }
      total_pages += heap.value()->FreeSpaceSnapshot().size();
    }
    database->buffer().ResetStats();
    for (const CsvTable& table : tables) {
      CHECK(ScanSorted(*database, table.schema.name) == table.rows);
    }
    const auto stats = database->buffer().Stats();
    CHECK_EQ(stats.page_reads, stats.misses);
    CHECK_EQ(stats.hits + stats.misses,
             static_cast<std::uint64_t>(total_pages));
  }
}

TEST(test_paged_environment_reuses_space_after_deletes) {
  if (!HasData()) {
    SKIP("data/ is missing");
  }
  const std::vector<CsvTable> tables = LoadAllCsv();
  TempDir directory;
  BuildDatabase(directory.path(), tables);
  auto database = OpenDatabase(directory.path(), ReplacementPolicyKind::kLru);
  if (database == nullptr) {
    return;
  }
  const CsvTable& posts = tables[1];
  auto heap = database->Table("posts");
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
  CHECK(ScanSorted(*database, "posts") == posts.rows);
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
  if (!HasData()) {
    SKIP("data/ is missing");
  }
  const std::vector<CsvTable> tables = LoadAllCsv();
  TempDir directory;
  BuildDatabase(directory.path(), tables);
  auto database = OpenDatabase(directory.path(), ReplacementPolicyKind::kLru);
  if (database == nullptr) {
    return;
  }

  // ADD COLUMN on the 10000-row posts table: every row gets the default.
  CHECK_OK(database->AddColumn(
      "posts", strata::domain::ColumnDef{"flag", strata::domain::DataType::kInt,
                                         Value(std::int64_t{0})}));
  std::vector<Row> posts_with_flag = tables[1].rows;
  for (Row& row : posts_with_flag) {
    row.push_back(Value(std::int64_t{0}));
  }
  CHECK(ScanSorted(*database, "posts") == posts_with_flag);
  const strata::paged::PageId pages_after_alter =
      database->buffer().PageCount();

  // DROP TABLE and a new load of the same data reuse the freed pages.
  CHECK_OK(database->DropTable("engagements"));
  CHECK_OK(database->CreateTable(tables[2].schema));
  auto heap = database->Table("engagements");
  CHECK_OK(heap);
  if (!heap.ok()) {
    return;
  }
  for (const Row& row : tables[2].rows) {
    CHECK_OK(heap.value()->Insert(row));
  }
  CHECK(database->buffer().PageCount() <= pages_after_alter + 2);
  CHECK(strata::testing::EveryPageIsAccountedFor(*database));
  CHECK(ScanSorted(*database, "users") == tables[0].rows);
  CHECK(ScanSorted(*database, "engagements") == tables[2].rows);

  CHECK_OK(database->Close());
  database = OpenDatabase(directory.path(), ReplacementPolicyKind::kTwoQ);
  if (database == nullptr) {
    return;
  }
  CHECK(ScanSorted(*database, "posts") == posts_with_flag);
  CHECK(ScanSorted(*database, "engagements") == tables[2].rows);
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
