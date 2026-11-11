#include "flatfiledb/bootstrap.h"

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "flatfiledb/application.h"
#include "flatfiledb/csv_file_source.h"
#include "flatfiledb/directory_lock.h"
#include "flatfiledb/flat_file_engine.h"
#include "flatfiledb/infrastructure.h"

namespace flatfiledb::bootstrap {

std::variant<OpenedDatabase, std::string> OpenFlatFileDatabase(
    const std::string& directory, bool is_parallel,
    const OpenedDatabase* current) {
  std::error_code error;
  bool is_created = false;
  std::filesystem::path given(directory);
  if (!std::filesystem::exists(given, error)) {
    // A path that ends in a separator names the directory before it.
    if (!given.has_filename()) {
      given = given.parent_path();
    }
    std::filesystem::path parent = given.parent_path();
    if (parent.empty()) {
      parent = ".";
    }
    if (!std::filesystem::is_directory(parent, error)) {
      return std::string("no such directory");
    }
    std::filesystem::create_directory(given, error);
    if (error) {
      return std::string("cannot create directory");
    }
    is_created = true;
  } else if (!std::filesystem::is_directory(given, error)) {
    return std::string("not a directory");
  }
  std::filesystem::path normalized = std::filesystem::canonical(given, error);
  if (error) {
    return std::string("no such directory");
  }
  const std::string path = normalized.string();

  std::shared_ptr<void> lock;
  if (current != nullptr && current->directory == path) {
    lock = current->lock;
  } else {
    infrastructure::LockOutcome outcome =
        infrastructure::DirectoryLock::Acquire(path);
    if (outcome.error) {
      return *outcome.error;
    }
    lock = outcome.lock;
  }

  auto repository = std::make_unique<infrastructure::CsvRepository>(path);
  auto engine =
      std::make_unique<infrastructure::FlatFileEngine>(std::move(repository));
  infrastructure::LoadResult loaded = engine->Load(is_parallel);
  if (!loaded.is_success) {
    return loaded.error;
  }

  std::vector<TableCount> tables;
  for (const infrastructure::TableCount& table : loaded.tables) {
    tables.push_back(
        TableCount{table.name, table.row_count, table.rejected_lines});
  }
  return OpenedDatabase{
      std::make_unique<application::QueryService>(
          std::move(engine), std::make_unique<infrastructure::FileCsvSource>()),
      path, std::move(lock), std::move(tables), is_created};
}

}  // namespace flatfiledb::bootstrap
