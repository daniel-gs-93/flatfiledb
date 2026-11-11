// Integration tests of OpenFlatFileDatabase against real database directories
// (docs/TestDocument.md section 3.2).

#include <unistd.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "flatfiledb/bootstrap.h"
#include "flatfiledb/directory_lock.h"
#include "test_support.h"

namespace {

namespace bootstrap = flatfiledb::bootstrap;
namespace domain = flatfiledb::domain;
namespace infrastructure = flatfiledb::infrastructure;
namespace testing = flatfiledb::testing;

using OpenOutcome = std::variant<bootstrap::OpenedDatabase, std::string>;

// The opened database, or null after reporting why the open failed.
std::unique_ptr<bootstrap::OpenedDatabase> MustOpen(
    const std::filesystem::path& directory, bool is_parallel,
    const bootstrap::OpenedDatabase* current = nullptr) {
  OpenOutcome outcome =
      bootstrap::OpenFlatFileDatabase(directory.string(), is_parallel, current);
  if (const auto* reason = std::get_if<std::string>(&outcome)) {
    testing::ReportFailure(__FILE__, __LINE__, "open failed: " + *reason);
    return nullptr;
  }
  return std::make_unique<bootstrap::OpenedDatabase>(
      std::move(std::get<bootstrap::OpenedDatabase>(outcome)));
}

// The reason the open failed, or "<opened>" after reporting that it succeeded.
std::string OpenFailure(const std::string& directory,
                        const bootstrap::OpenedDatabase* current = nullptr) {
  OpenOutcome outcome =
      bootstrap::OpenFlatFileDatabase(directory, false, current);
  if (const auto* reason = std::get_if<std::string>(&outcome)) {
    return *reason;
  }
  testing::ReportFailure(__FILE__, __LINE__, "the open succeeded");
  return "<opened>";
}

// Whether another Acquire of the directory is refused right now.
bool IsLockHeld(const std::filesystem::path& directory) {
  infrastructure::LockOutcome outcome =
      infrastructure::DirectoryLock::Acquire(directory.string());
  return outcome.error.has_value();
}

}  // namespace

TEST(test_bootstrap_opens_database_and_reports_counts) {
  // Arrange
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());
  const bool load_modes[] = {false, true};

  for (bool is_parallel : load_modes) {
    // Act
    std::unique_ptr<bootstrap::OpenedDatabase> opened =
        MustOpen(dir.path(), is_parallel);

    // Assert
    CHECK(opened != nullptr);
    if (opened == nullptr) {
      continue;
    }
    CHECK_EQ(opened->tables.size(), std::size_t{3});
    if (opened->tables.size() == 3) {
      CHECK_EQ(opened->tables[0].name, std::string("users"));
      CHECK_EQ(opened->tables[0].row_count, std::size_t{4});
      CHECK_EQ(opened->tables[1].name, std::string("posts"));
      CHECK_EQ(opened->tables[1].row_count, std::size_t{3});
      CHECK_EQ(opened->tables[2].name, std::string("engagements"));
      CHECK_EQ(opened->tables[2].row_count, std::size_t{5});
    }
    CHECK_EQ(opened->directory,
             std::filesystem::canonical(dir.path()).string());
    CHECK(opened->service != nullptr);
    if (opened->service == nullptr) {
      continue;
    }
    domain::ExecutionResult result = testing::ExecuteSql(
        *opened->service, "SELECT username FROM users WHERE id = 1");
    const domain::ResultSet* result_set = testing::AsResultSet(result);
    CHECK(result_set != nullptr);
    if (result_set == nullptr) {
      continue;
    }
    CHECK_EQ(testing::RowsAsText(*result_set),
             std::vector<std::string>({"alice"}));
  }
}

TEST(test_bootstrap_missing_file_returns_description) {
  // Arrange
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());
  std::error_code error;
  std::filesystem::remove(dir.path() / "posts.csv", error);
  CHECK(!error);

  // Act
  const std::string reason = OpenFailure(dir.path().string());

  // Assert
  CHECK_EQ(reason, std::string("cannot read posts.csv"));
}

TEST(test_bootstrap_missing_directory_returns_description) {
  testing::TempDir dir;
  testing::WriteFile(dir.path() / "regular.txt", "x");

  const std::string missing_parent =
      OpenFailure((dir.path() / "nosuch" / "db").string());
  const std::string regular_file =
      OpenFailure((dir.path() / "regular.txt").string());

  CHECK_EQ(missing_parent, std::string("no such directory"));
  CHECK_EQ(regular_file, std::string("not a directory"));
  CHECK(!std::filesystem::exists(dir.path() / "nosuch"));
  CHECK_EQ(testing::ReadFile(dir.path() / "regular.txt"), std::string("x"));
}

TEST(test_bootstrap_open_creates_missing_directory) {
  testing::TempDir dir;

  OpenOutcome created = bootstrap::OpenFlatFileDatabase(
      (dir.path() / "fresh").string(), false, nullptr);
  OpenOutcome trailing = bootstrap::OpenFlatFileDatabase(
      (dir.path() / "fresh2").string() + "/", false, nullptr);
  OpenOutcome existing =
      bootstrap::OpenFlatFileDatabase(dir.path().string(), false, nullptr);

  auto* first = std::get_if<bootstrap::OpenedDatabase>(&created);
  auto* second = std::get_if<bootstrap::OpenedDatabase>(&trailing);
  auto* third = std::get_if<bootstrap::OpenedDatabase>(&existing);
  CHECK(first != nullptr && second != nullptr && third != nullptr);
  if (first == nullptr || second == nullptr || third == nullptr) {
    return;
  }
  CHECK(first->is_created);
  CHECK(second->is_created);
  CHECK(!third->is_created);
  CHECK(first->tables.empty());
  CHECK(std::filesystem::is_directory(dir.path() / "fresh"));
  CHECK(std::filesystem::is_directory(dir.path() / "fresh2"));
  CHECK(std::filesystem::exists(dir.path() / "fresh" / ".flatfiledb.lock"));
}

TEST(test_bootstrap_create_failure_returns_description) {
  if (::geteuid() == 0) {
    SKIP("running as root: directory permissions do not apply");
  }
  testing::TempDir dir;
  std::filesystem::permissions(
      dir.path(),
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace);

  const std::string reason = OpenFailure((dir.path() / "fresh").string());

  CHECK_EQ(reason, std::string("cannot create directory"));
  CHECK(!std::filesystem::exists(dir.path() / "fresh"));
}

TEST(test_bootstrap_open_reports_rejected_lines) {
  testing::TempDir dir;
  testing::WriteFixtureB(dir.path());

  OpenOutcome outcome =
      bootstrap::OpenFlatFileDatabase(dir.path().string(), false, nullptr);

  auto* opened = std::get_if<bootstrap::OpenedDatabase>(&outcome);
  CHECK(opened != nullptr);
  if (opened == nullptr || opened->tables.size() != 3) {
    return;
  }
  CHECK_EQ(opened->tables[0].name, std::string("users"));
  CHECK_EQ(opened->tables[0].row_count, std::size_t{2});
  CHECK_EQ(opened->tables[0].rejected_lines, std::vector<std::size_t>({3, 4}));
  CHECK_EQ(opened->tables[1].rejected_lines, std::vector<std::size_t>({3}));
  CHECK(opened->tables[2].rejected_lines.empty());
}

TEST(test_bootstrap_locked_directory_returns_description) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());
  infrastructure::LockOutcome held =
      infrastructure::DirectoryLock::Acquire(dir.path().string());
  CHECK(held.lock != nullptr);

  const std::string reason = OpenFailure(dir.path().string());

  CHECK_EQ(reason, std::string("database is locked by another process"));
}

TEST(test_bootstrap_reopen_same_directory_reuses_lock) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());
  std::unique_ptr<bootstrap::OpenedDatabase> first =
      MustOpen(dir.path(), false);
  CHECK(first != nullptr);
  if (first == nullptr) {
    return;
  }

  std::unique_ptr<bootstrap::OpenedDatabase> second =
      MustOpen(dir.path(), false, first.get());

  CHECK(second != nullptr);
  if (second == nullptr) {
    return;
  }
  CHECK(first->lock != nullptr);
  CHECK(second->lock == first->lock);
  CHECK(IsLockHeld(dir.path()));
  first.reset();
  CHECK(IsLockHeld(dir.path()));
  second.reset();
  CHECK(!IsLockHeld(dir.path()));
}

TEST(test_bootstrap_failed_open_keeps_current_lock_and_releases_new_one) {
  testing::TempDir good_dir;
  testing::WriteFixtureA(good_dir.path());
  testing::TempDir broken_dir;
  testing::WriteFixtureA(broken_dir.path());
  std::error_code error;
  std::filesystem::remove(broken_dir.path() / "posts.csv", error);
  std::unique_ptr<bootstrap::OpenedDatabase> current =
      MustOpen(good_dir.path(), false);
  CHECK(current != nullptr);
  if (current == nullptr) {
    return;
  }

  const std::string reason =
      OpenFailure(broken_dir.path().string(), current.get());

  CHECK_EQ(reason, std::string("cannot read posts.csv"));
  CHECK(IsLockHeld(good_dir.path()));
  CHECK(!IsLockHeld(broken_dir.path()));
}

TEST(test_bootstrap_empty_directory_has_no_tables) {
  testing::TempDir dir;

  std::unique_ptr<bootstrap::OpenedDatabase> opened =
      MustOpen(dir.path(), false);

  CHECK(opened != nullptr);
  if (opened == nullptr) {
    return;
  }
  CHECK(opened->tables.empty());
  domain::ExecutionResult result = testing::ExecuteSql(
      *opened->service, "CREATE TABLE t (id INT PRIMARY KEY)");
  CHECK(testing::AsCommandComplete(result) != nullptr);
}
