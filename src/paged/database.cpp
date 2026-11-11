#include "flatfiledb/paged/database.h"

#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "flatfiledb/paged/byte_codec.h"
#include "flatfiledb/paged/slotted_page.h"

namespace flatfiledb::paged {

namespace {

constexpr char kMagic[layout::kMagicSize] = {'P', 'A', 'G', 'E',
                                             'D', 'D', 'B', '\0'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr PageId kSysTablesPage = 1;
constexpr PageId kSysColumnsPage = 2;
constexpr std::uint32_t kFirstUserTableId = 3;
constexpr PageId kMinimumPages = 3;
constexpr const char* kFileName = "paged.db";

struct FileHeader {
  PageId sys_tables_first_page;
  PageId sys_columns_first_page;
};

PageError Corrupt(const std::string& detail) {
  return MakeError(PageErrorCode::kCorrupt, kFileName + (": " + detail));
}

// Writes the first three pages of a new database through the buffer pool and
// flushes them: the file header, and the first pages of sys_tables and
// sys_columns.
Status InitializeNewDatabase(BufferManager& buffer) {
  {
    PageResult<PageGuard> created = buffer.NewPage();
    if (!created.ok()) {
      return created.error();
    }
    PageGuard guard = std::move(created).value();
    std::uint8_t* page = guard.mutable_data();
    page[layout::kPageTypeField] =
        static_cast<std::uint8_t>(PageType::kFileHeader);
    std::memcpy(page + layout::kMagic, kMagic, layout::kMagicSize);
    WriteLe32(page + layout::kFormatVersion, kFormatVersion);
    WriteLe32(page + layout::kFilePageSize,
              static_cast<std::uint32_t>(kPageSize));
    WriteLe32(page + layout::kNextTableId, kFirstUserTableId);
    WriteLe32(page + layout::kSysTablesFirstPage, kSysTablesPage);
    WriteLe32(page + layout::kSysColumnsFirstPage, kSysColumnsPage);
    guard.MarkDirty();
  }
  const std::uint32_t system_table_ids[] = {1, 2};
  for (std::uint32_t table_id : system_table_ids) {
    PageResult<PageGuard> created = buffer.NewPage();
    if (!created.ok()) {
      return created.error();
    }
    PageGuard guard = std::move(created).value();
    SlottedPage::Format(guard.mutable_data(), guard.page_id(), table_id);
    guard.MarkDirty();
  }
  return buffer.FlushAll();
}

// Checks page 0 of an existing file in the order of design document 2.9 and
// returns the first pages of the system tables.
PageResult<FileHeader> ValidateExistingFile(IPageStore& store) {
  const PageId page_count = store.PageCount();
  if (page_count < kMinimumPages) {
    return Corrupt("size of " + std::to_string(page_count) +
                   " pages is below the minimum of " +
                   std::to_string(kMinimumPages));
  }
  std::array<std::uint8_t, kPageSize> header{};
  if (Status status = store.ReadPage(0, header.data())) {
    return *status;
  }
  if (std::memcmp(header.data() + layout::kMagic, kMagic, layout::kMagicSize) !=
      0) {
    return Corrupt("magic is not PAGEDDB");
  }
  const std::uint32_t version =
      ReadLe32(header.data() + layout::kFormatVersion);
  if (version != kFormatVersion) {
    return Corrupt("format_version " + std::to_string(version) +
                   " is not supported (expected " +
                   std::to_string(kFormatVersion) + ")");
  }
  const std::uint32_t page_size =
      ReadLe32(header.data() + layout::kFilePageSize);
  if (page_size != kPageSize) {
    return Corrupt("page_size " + std::to_string(page_size) + " differs from " +
                   std::to_string(kPageSize));
  }
  // page_type is checked before VerifyHeader: a page 0 that claims another
  // type would otherwise be judged by the rules of that type.
  if (header[layout::kPageTypeField] !=
      static_cast<std::uint8_t>(PageType::kFileHeader)) {
    return Corrupt("page 0 has page_type " +
                   std::to_string(header[layout::kPageTypeField]) +
                   " instead of 1");
  }
  if (Status status = SlottedPage::VerifyHeader(header.data(), 0)) {
    return *status;
  }
  FileHeader result;
  result.sys_tables_first_page =
      ReadLe32(header.data() + layout::kSysTablesFirstPage);
  result.sys_columns_first_page =
      ReadLe32(header.data() + layout::kSysColumnsFirstPage);
  if (result.sys_tables_first_page >= page_count) {
    return Corrupt("sys_tables_first_page " +
                   std::to_string(result.sys_tables_first_page) +
                   " is outside the file (" + std::to_string(page_count) +
                   " pages)");
  }
  if (result.sys_columns_first_page >= page_count) {
    return Corrupt("sys_columns_first_page " +
                   std::to_string(result.sys_columns_first_page) +
                   " is outside the file (" + std::to_string(page_count) +
                   " pages)");
  }
  const std::uint32_t next_table_id =
      ReadLe32(header.data() + layout::kNextTableId);
  if (next_table_id < kFirstUserTableId) {
    return Corrupt("next_table_id " + std::to_string(next_table_id) +
                   " is below " + std::to_string(kFirstUserTableId));
  }
  return result;
}

}  // namespace

Database::Database(std::unique_ptr<IPageStore> store,
                   std::unique_ptr<BufferManager> buffer,
                   std::unique_ptr<Catalog> catalog)
    : store_(std::move(store)),
      buffer_(std::move(buffer)),
      catalog_(std::move(catalog)) {}

Database::~Database() {
  if (buffer_ == nullptr || is_closed_) {
    return;
  }
  if (Status status = buffer_->FlushAll()) {
    std::cerr << "paged: failed to write the database on destruction: "
              << status->message << "\n";
  }
}

PageResult<Database> Database::Open(const std::filesystem::path& directory,
                                    const DatabaseOptions& options) {
  std::error_code directory_error;
  if (!std::filesystem::is_directory(directory, directory_error)) {
    return MakeError(PageErrorCode::kIo,
                     directory.string() + " is not an existing directory");
  }
  PageResult<FilePageStore> opened =
      FilePageStore::Open(directory / kFileName, OpenMode::kOpenOrCreate);
  if (!opened.ok()) {
    return opened.error();
  }
  return OpenWithStore(
      std::make_unique<FilePageStore>(std::move(opened).value()), options);
}

PageResult<Database> Database::OpenWithStore(std::unique_ptr<IPageStore> store,
                                             const DatabaseOptions& options) {
  PageResult<std::unique_ptr<BufferManager>> created =
      BufferManager::Create(*store, options.pool_frames, options.policy);
  if (!created.ok()) {
    return created.error();
  }
  std::unique_ptr<BufferManager> buffer = std::move(created).value();

  FileHeader header{kSysTablesPage, kSysColumnsPage};
  if (store->PageCount() == 0) {
    if (Status status = InitializeNewDatabase(*buffer)) {
      return *status;
    }
  } else {
    PageResult<FileHeader> validated = ValidateExistingFile(*store);
    if (!validated.ok()) {
      return validated.error();
    }
    header = validated.value();
  }
  auto catalog = std::make_unique<Catalog>(
      *buffer, header.sys_tables_first_page, header.sys_columns_first_page);
  if (Status status = catalog->Load()) {
    return *status;
  }
  return Database(std::move(store), std::move(buffer), std::move(catalog));
}

PageResult<TableHeap*> Database::Table(const std::string& name) {
  std::lock_guard<std::mutex> tables_lock(*tables_mutex_);
  if (is_closed_) {
    return MakeError(PageErrorCode::kClosed, "the database is closed");
  }
  const std::string key = domain::ToLower(name);
  auto cached = tables_.find(key);
  if (cached != tables_.end()) {
    return cached->second.get();
  }
  const std::optional<domain::TableSchema> schema = catalog_->FindTable(name);
  const std::optional<std::uint32_t> table_id = catalog_->TableId(name);
  const std::optional<PageId> first_page = catalog_->FirstPage(name);
  if (!schema.has_value() || !table_id.has_value() || !first_page.has_value()) {
    return MakeError(PageErrorCode::kNotFound, "no table named " + name);
  }
  PageResult<TableHeap> opened =
      TableHeap::Open(*buffer_, *schema, *table_id, *first_page);
  if (!opened.ok()) {
    return opened.error();
  }
  auto heap = std::make_unique<TableHeap>(std::move(opened).value());
  TableHeap* const pointer = heap.get();
  tables_.emplace(key, std::move(heap));
  return pointer;
}

PageResult<domain::TableSchema> Database::CreateTable(
    const domain::TableSchema& schema) {
  {
    std::lock_guard<std::mutex> tables_lock(*tables_mutex_);
    if (is_closed_) {
      return MakeError(PageErrorCode::kClosed, "the database is closed");
    }
  }
  return catalog_->CreateTable(schema);
}

Status Database::Close() {
  std::lock_guard<std::mutex> tables_lock(*tables_mutex_);
  if (is_closed_) {
    return std::nullopt;
  }
  if (Status status = buffer_->FlushAll()) {
    return status;
  }
  tables_.clear();
  is_closed_ = true;
  return std::nullopt;
}

}  // namespace flatfiledb::paged
