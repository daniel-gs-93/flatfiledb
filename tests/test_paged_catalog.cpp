#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "flatfiledb/domain/value.h"
#include "flatfiledb/paged/byte_codec.h"
#include "flatfiledb/paged/catalog.h"
#include "flatfiledb/paged/database.h"
#include "flatfiledb/paged/page_store.h"
#include "flatfiledb/paged/table_heap.h"
#include "test_harness.h"
#include "test_paged_support.h"

// Catalog over a Database on a MemoryPageStore.

namespace {

using flatfiledb::domain::ColumnDef;
using flatfiledb::domain::DataType;
using flatfiledb::domain::ForeignKey;
using flatfiledb::domain::Row;
using flatfiledb::domain::TableSchema;
using flatfiledb::domain::Value;
using flatfiledb::paged::Catalog;
using flatfiledb::paged::Database;
using flatfiledb::paged::DatabaseOptions;
using flatfiledb::paged::LatchMode;
using flatfiledb::paged::MemoryPageStore;
using flatfiledb::paged::PageGuard;
using flatfiledb::paged::PageId;
using flatfiledb::paged::Rid;
using flatfiledb::paged::TableHeap;
using Operation = flatfiledb::paged::MemoryPageStore::Operation;

struct OpenedDatabase {
  MemoryPageStore* store = nullptr;  // owned by database
  std::unique_ptr<Database> database;
};

OpenedDatabase OpenMemoryDatabase(std::size_t frames = 16) {
  auto store = std::make_unique<MemoryPageStore>();
  MemoryPageStore* const observer = store.get();
  DatabaseOptions options;
  options.pool_frames = frames;
  auto opened = Database::OpenWithStore(std::move(store), options);
  CHECK_OK(opened);
  OpenedDatabase result;
  if (opened.ok()) {
    result.store = observer;
    result.database = std::make_unique<Database>(std::move(opened).value());
  }
  return result;
}

TableSchema BasicSchema(const std::string& name) {
  TableSchema schema;
  schema.name = name;
  schema.columns = {{"id", DataType::kInt}, {"name", DataType::kText}};
  schema.primary_key_index = 0;
  return schema;
}

std::uint64_t BitsOf(double number) {
  return std::bit_cast<std::uint64_t>(number);
}

bool DefaultsEqual(const std::optional<Value>& left,
                   const std::optional<Value>& right) {
  if (left.has_value() != right.has_value()) {
    return false;
  }
  if (!left.has_value()) {
    return true;
  }
  if (left->index() != right->index()) {
    return false;
  }
  if (std::holds_alternative<double>(*left)) {
    return BitsOf(std::get<double>(*left)) == BitsOf(std::get<double>(*right));
  }
  return *left == *right;
}

bool ForeignKeysEqual(const std::vector<ForeignKey>& left,
                      const std::vector<ForeignKey>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (left[i].column_index != right[i].column_index ||
        left[i].ref_table != right[i].ref_table ||
        left[i].ref_column_index != right[i].ref_column_index) {
      return false;
    }
  }
  return true;
}

bool SchemasEqual(const TableSchema& left, const TableSchema& right) {
  if (left.name != right.name || left.columns.size() != right.columns.size() ||
      left.primary_key_index != right.primary_key_index ||
      left.unique_indexes != right.unique_indexes ||
      left.auto_increment_index != right.auto_increment_index ||
      !ForeignKeysEqual(left.foreign_keys, right.foreign_keys)) {
    return false;
  }
  for (std::size_t i = 0; i < left.columns.size(); ++i) {
    if (left.columns[i].name != right.columns[i].name ||
        left.columns[i].type != right.columns[i].type ||
        !DefaultsEqual(left.columns[i].default_value,
                       right.columns[i].default_value)) {
      return false;
    }
  }
  return true;
}

// The number of rows in one of the system tables (1 = sys_tables, 2 =
// sys_columns, whose first pages are 1 and 2 in a new database).
std::size_t SystemRowCount(Database& database, std::uint32_t table_id) {
  auto heap = TableHeap::Open(
      database.buffer(),
      table_id == 1 ? Catalog::SysTablesSchema() : Catalog::SysColumnsSchema(),
      table_id, table_id);
  CHECK_OK(heap);
  if (!heap.ok()) {
    return 0;
  }
  std::size_t count = 0;
  auto scan = heap.value().Scan();
  while (true) {
    auto next = scan.Next();
    if (!next.ok() || !next.value().has_value()) {
      break;
    }
    ++count;
  }
  return count;
}

}  // namespace

TEST(test_paged_catalog_round_trips_schemas) {
  OpenedDatabase env = OpenMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  CHECK_OK(database.CreateTable(BasicSchema("b")));

  TableSchema schema;
  schema.name = "T";
  schema.columns = {
      ColumnDef{"id", DataType::kInt, std::nullopt},
      ColumnDef{"email", DataType::kText, std::nullopt},
      ColumnDef{"owner_id", DataType::kInt,
                Value(std::numeric_limits<std::int64_t>::min())},
      ColumnDef{"score", DataType::kDouble, Value(0.1)},
      ColumnDef{"offset", DataType::kDouble, Value(-0.0)},
      ColumnDef{"note", DataType::kText, Value(std::string("it's\nok"))},
      ColumnDef{"tag", DataType::kText, Value(std::string())}};
  schema.primary_key_index = 0;
  schema.unique_indexes = {1};
  schema.foreign_keys = {ForeignKey{2, "b", 0}};
  schema.auto_increment_index = 0;
  auto created = database.CreateTable(schema);
  CHECK_OK(created);

  Catalog fresh(database.buffer(), 1, 2);
  CHECK_OK(fresh.Load());
  auto found = fresh.FindTable("t");  // names are case-insensitive
  CHECK(found.has_value());
  if (found.has_value()) {
    CHECK(SchemasEqual(*found, schema));
  }
  CHECK(fresh.FindTable("missing") == std::nullopt);
  CHECK(fresh.FindTable("sys_tables") == std::nullopt);
  const std::vector<std::string> expected_names = {"b", "T"};
  CHECK(fresh.TableNames() == expected_names);
  CHECK(fresh.TableId("T").has_value());
  CHECK(fresh.FirstPage("T").has_value());
}

TEST(test_paged_catalog_create_table_validation) {
  OpenedDatabase env = OpenMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  CHECK_OK(database.CreateTable(BasicSchema("users")));

  struct Case {
    TableSchema schema;
    const char* expected_error;
  };
  std::vector<Case> cases;
  cases.push_back({BasicSchema("users"), "kAlreadyExists"});
  cases.push_back({BasicSchema("USERS"), "kAlreadyExists"});
  cases.push_back({BasicSchema("sys_tables"), "kInvalidArgument"});
  cases.push_back({BasicSchema("SYS_x"), "kInvalidArgument"});
  cases.push_back({BasicSchema(""), "kInvalidArgument"});
  cases.push_back({BasicSchema("1a"), "kInvalidArgument"});
  cases.push_back({BasicSchema("a b"), "kInvalidArgument"});
  TableSchema no_columns = BasicSchema("t1");
  no_columns.columns.clear();
  cases.push_back({no_columns, "kInvalidArgument"});
  TableSchema duplicate_column = BasicSchema("t2");
  duplicate_column.columns = {{"a", DataType::kInt}, {"A", DataType::kInt}};
  cases.push_back({duplicate_column, "kInvalidArgument"});
  TableSchema bad_primary_key = BasicSchema("t3");
  bad_primary_key.primary_key_index = 5;
  cases.push_back({bad_primary_key, "kInvalidArgument"});
  TableSchema auto_on_text = BasicSchema("t4");
  auto_on_text.auto_increment_index = 1;
  cases.push_back({auto_on_text, "kInvalidArgument"});
  TableSchema bad_unique = BasicSchema("t5");
  bad_unique.unique_indexes = {5};
  cases.push_back({bad_unique, "kInvalidArgument"});
  TableSchema unsorted_unique = BasicSchema("t6");
  unsorted_unique.unique_indexes = {1, 0};
  cases.push_back({unsorted_unique, "kInvalidArgument"});
  TableSchema bad_foreign_key = BasicSchema("t7");
  bad_foreign_key.foreign_keys = {ForeignKey{5, "users", 0}};
  cases.push_back({bad_foreign_key, "kInvalidArgument"});

  const PageId pages_before = env.store->PageCount();
  const std::size_t tables_before = SystemRowCount(database, 1);
  const std::size_t columns_before = SystemRowCount(database, 2);
  for (const Case& item : cases) {
    auto result = database.CreateTable(item.schema);
    CHECK_EQ(flatfiledb::testing::ErrorName(result),
             std::string(item.expected_error));
    CHECK_EQ(env.store->PageCount(), pages_before);
    CHECK_EQ(SystemRowCount(database, 1), tables_before);
    CHECK_EQ(SystemRowCount(database, 2), columns_before);
  }
  const std::vector<std::string> names = {"users"};
  CHECK(database.catalog().TableNames() == names);
}

TEST(test_paged_catalog_create_table_undoes_partial_rows) {
  OpenedDatabase env = OpenMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  TableSchema first;
  first.name = "wide_a";
  TableSchema second;
  second.name = "wide_b";
  for (int i = 0; i < 100; ++i) {
    first.columns.push_back({"c" + std::to_string(1000 + i), DataType::kInt});
    second.columns.push_back({"c" + std::to_string(1000 + i), DataType::kInt});
  }
  first.primary_key_index = 0;
  second.primary_key_index = 0;
  CHECK_OK(database.CreateTable(first));

  const std::size_t tables_before = SystemRowCount(database, 1);
  const std::size_t columns_before = SystemRowCount(database, 2);
  // The first allocation is wide_b's first page; the second is a new page of
  // sys_columns, which the 100 column rows of wide_b need.
  env.store->InjectFailure(Operation::kAllocate, 2);
  CHECK_ERROR(database.CreateTable(second), kIo);
  CHECK_EQ(SystemRowCount(database, 1), tables_before);
  CHECK_EQ(SystemRowCount(database, 2), columns_before);
  Catalog fresh(database.buffer(), 1, 2);
  CHECK_OK(fresh.Load());
  CHECK(fresh.FindTable("wide_b") == std::nullopt);
  CHECK(fresh.FindTable("wide_a").has_value());
  env.store->ClearInjectedFailures();
  CHECK_OK(database.CreateTable(second));
}

namespace {

enum class SystemDamage {
  kNone,
  kDuplicateTableId,
  kDuplicateName,
  kFirstPageOutsideFile,
  kMissingPosition,
  kBadColumnType,
  kBadDefaultKind,
  kBadPrimaryKeyIndex,
  kBadAutoIncrementIndex,
  kBadRefColumn,
  kNextTableIdTooLow,
  kOrphanColumnRow,
  kBadDefaultText
};

// The row of a system table whose column `column` equals `value` and, when
// position is given, whose column 1 equals position (sys_columns).
std::optional<flatfiledb::paged::ScanItem> FindSystemRow(
    TableHeap& heap, std::size_t column, std::int64_t value,
    std::optional<std::int64_t> position = std::nullopt) {
  auto scan = heap.Scan();
  while (true) {
    auto next = scan.Next();
    if (!next.ok() || !next.value().has_value()) {
      return std::nullopt;
    }
    const Row& row = next.value()->row;
    if (std::get<std::int64_t>(row[column]) != value) {
      continue;
    }
    if (position.has_value() && std::get<std::int64_t>(row[1]) != *position) {
      continue;
    }
    return std::move(*next.value());
  }
}

void SetIntColumn(TableHeap& heap, const flatfiledb::paged::ScanItem& item,
                  std::size_t column, std::int64_t value) {
  Row row = item.row;
  row[column] = Value(value);
  CHECK_OK(heap.Update(item.rid, row));
}

// Outcome of Catalog::Load after the system tables or the file header were
// damaged: "ok" or the error name and message.
std::string LoadAfterDamage(SystemDamage damage) {
  OpenedDatabase env = OpenMemoryDatabase();
  if (env.database == nullptr) {
    return "no database";
  }
  Database& database = *env.database;
  TableSchema schema = BasicSchema("T");
  schema.columns.push_back({"ref", DataType::kInt});
  schema.foreign_keys = {ForeignKey{2, "b", 0}};
  CHECK_OK(database.CreateTable(schema));  // table_id 3

  auto tables =
      TableHeap::Open(database.buffer(), Catalog::SysTablesSchema(), 1, 1);
  auto columns =
      TableHeap::Open(database.buffer(), Catalog::SysColumnsSchema(), 2, 2);
  CHECK_OK(tables);
  CHECK_OK(columns);
  if (!tables.ok() || !columns.ok()) {
    return "no system tables";
  }
  TableHeap& sys_tables = tables.value();
  TableHeap& sys_columns = columns.value();
  const auto table_row = FindSystemRow(sys_tables, 0, 3);
  CHECK(table_row.has_value());
  if (!table_row.has_value()) {
    return "no row";
  }

  switch (damage) {
    case SystemDamage::kNone:
      break;
    case SystemDamage::kDuplicateTableId:
      CHECK_OK(sys_tables.Insert(Row{
          Value(std::int64_t{3}), Value(std::string("other")),
          table_row->row[2], Value(std::int64_t{0}), Value(std::int64_t{-1})}));
      break;
    case SystemDamage::kDuplicateName:
      CHECK_OK(sys_tables.Insert(Row{
          Value(std::int64_t{4}), Value(std::string("t")), table_row->row[2],
          Value(std::int64_t{0}), Value(std::int64_t{-1})}));
      break;
    case SystemDamage::kFirstPageOutsideFile:
      SetIntColumn(sys_tables, *table_row, 2,
                   static_cast<std::int64_t>(env.store->PageCount()));
      break;
    case SystemDamage::kMissingPosition: {
      const auto doomed = FindSystemRow(sys_columns, 0, 3, 1);
      CHECK(doomed.has_value());
      if (doomed.has_value()) {
        CHECK_OK(sys_columns.Delete(doomed->rid));
      }
      break;
    }
    case SystemDamage::kBadColumnType: {
      const auto target = FindSystemRow(sys_columns, 0, 3, 0);
      CHECK(target.has_value());
      if (target.has_value()) {
        SetIntColumn(sys_columns, *target, 3, 9);
      }
      break;
    }
    case SystemDamage::kBadDefaultKind: {
      const auto target = FindSystemRow(sys_columns, 0, 3, 0);
      CHECK(target.has_value());
      if (target.has_value()) {
        SetIntColumn(sys_columns, *target, 4, 9);
      }
      break;
    }
    case SystemDamage::kBadPrimaryKeyIndex:
      SetIntColumn(sys_tables, *table_row, 3, 3);
      break;
    case SystemDamage::kBadAutoIncrementIndex:
      SetIntColumn(sys_tables, *table_row, 4, 7);
      break;
    case SystemDamage::kBadRefColumn: {
      const auto target = FindSystemRow(sys_columns, 0, 3, 2);
      CHECK(target.has_value());
      if (target.has_value()) {
        SetIntColumn(sys_columns, *target, 8, -1);
      }
      break;
    }
    case SystemDamage::kOrphanColumnRow:
      CHECK_OK(sys_columns.Insert(Row{
          Value(std::int64_t{99}), Value(std::int64_t{0}),
          Value(std::string("x")), Value(std::int64_t{0}),
          Value(std::int64_t{0}), Value(std::string()), Value(std::int64_t{0}),
          Value(std::string()), Value(std::int64_t{-1})}));
      break;
    case SystemDamage::kBadDefaultText: {
      const auto target = FindSystemRow(sys_columns, 0, 3, 0);
      CHECK(target.has_value());
      if (target.has_value()) {
        Row row = target->row;
        row[4] = Value(std::int64_t{1});     // default_kind: INT
        row[5] = Value(std::string("abc"));  // default_text: not a number
        CHECK_OK(sys_columns.Update(target->rid, row));
      }
      break;
    }
    case SystemDamage::kNextTableIdTooLow: {
      auto header = database.buffer().FetchPage(0, LatchMode::kExclusive);
      CHECK_OK(header);
      if (header.ok()) {
        PageGuard guard = std::move(header).value();
        flatfiledb::paged::WriteLe32(guard.mutable_data() + 48, 3);
        guard.MarkDirty();
      }
      break;
    }
  }
  Catalog fresh(database.buffer(), 1, 2);
  const flatfiledb::paged::Status loaded = fresh.Load();
  return loaded.has_value()
             ? std::string(flatfiledb::testing::ErrorCodeName(loaded->code)) +
                   ": " + loaded->message
             : "ok";
}

}  // namespace

TEST(test_paged_catalog_load_rejects_inconsistent_system_tables) {
  CHECK_EQ(LoadAfterDamage(SystemDamage::kNone), "ok");
  const SystemDamage damages[] = {
      SystemDamage::kDuplicateTableId,     SystemDamage::kDuplicateName,
      SystemDamage::kFirstPageOutsideFile, SystemDamage::kMissingPosition,
      SystemDamage::kBadColumnType,        SystemDamage::kBadDefaultKind,
      SystemDamage::kBadPrimaryKeyIndex,   SystemDamage::kBadAutoIncrementIndex,
      SystemDamage::kBadRefColumn,         SystemDamage::kNextTableIdTooLow,
      SystemDamage::kOrphanColumnRow,      SystemDamage::kBadDefaultText};
  for (SystemDamage damage : damages) {
    const std::string outcome = LoadAfterDamage(damage);
    CHECK(outcome.starts_with("kCorrupt"));
    if (!outcome.starts_with("kCorrupt")) {
      CHECK_EQ(outcome, std::string("kCorrupt"));  // shows which case failed
    }
  }
  CHECK(
      LoadAfterDamage(SystemDamage::kNextTableIdTooLow).find("next_table_id") !=
      std::string::npos);
}
