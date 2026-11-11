#include "flatfiledb/bootstrap.h"

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "flatfiledb/application.h"
#include "flatfiledb/directory_lock.h"
#include "flatfiledb/flat_file_engine.h"
#include "flatfiledb/infrastructure.h"

namespace flatfiledb::bootstrap {

std::variant<OpenedDatabase, std::string> OpenFlatFileDatabase(
    const std::string& directory, bool is_parallel,
    const OpenedDatabase* current) {
  std::error_code error;
  std::filesystem::path normalized =
      std::filesystem::canonical(directory, error);
  if (error || !std::filesystem::is_directory(normalized, error)) {
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
    tables.push_back(TableCount{table.name, table.row_count});
  }
  return OpenedDatabase{
      std::make_unique<application::QueryService>(std::move(engine)), path,
      std::move(lock), std::move(tables)};
}

}  // namespace flatfiledb::bootstrap
