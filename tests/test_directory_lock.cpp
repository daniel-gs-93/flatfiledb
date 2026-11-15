// Integration tests of DirectoryLock against real directories.

#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>

#include "strata/directory_lock.h"
#include "test_harness.h"
#include "test_support.h"

namespace {

namespace infrastructure = strata::infrastructure;
namespace testing = strata::testing;

}  // namespace

TEST(test_directory_lock_second_acquire_fails_until_release) {
  testing::TempDir dir;
  const std::string directory = dir.path().string();

  infrastructure::LockOutcome first =
      infrastructure::DirectoryLock::Acquire(directory);
  infrastructure::LockOutcome second =
      infrastructure::DirectoryLock::Acquire(directory);
  const bool is_first_held = first.lock != nullptr;
  first.lock.reset();
  infrastructure::LockOutcome third =
      infrastructure::DirectoryLock::Acquire(directory);

  CHECK(is_first_held);
  CHECK(!first.error.has_value());
  CHECK(second.lock == nullptr);
  CHECK(second.error.has_value());
  if (second.error) {
    CHECK_EQ(*second.error,
             std::string("database is locked by another process"));
  }
  CHECK(third.lock != nullptr);
  CHECK(!third.error.has_value());
}

TEST(test_directory_lock_unwritable_directory_is_not_needed) {
  if (::geteuid() == 0) {
    SKIP("permissions do not apply to root");
  }
  testing::TempDir dir;
  ::chmod(dir.path().c_str(), 0555);

  infrastructure::LockOutcome outcome =
      infrastructure::DirectoryLock::Acquire(dir.path().string());

  CHECK(outcome.lock == nullptr);
  CHECK(!outcome.error.has_value());
}
