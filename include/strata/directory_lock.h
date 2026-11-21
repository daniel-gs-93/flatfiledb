#ifndef STRATA_DIRECTORY_LOCK_H_
#define STRATA_DIRECTORY_LOCK_H_

#include <memory>
#include <optional>
#include <string>

namespace strata::infrastructure {

class DirectoryLock;

// The result of Acquire. error is set when the lock could not be taken; lock is
// empty with no error when none is needed (the directory is not writable).
struct LockOutcome {
  std::shared_ptr<DirectoryLock> lock;
  std::optional<std::string> error;
};

// An exclusive flock on `.strata.lock` in a database directory, held until
// the last owner of the object is destroyed (docs/DesignDocument.md section
// 2.14, ADR-0015). The lock file is never renamed or removed, so the lock
// survives the staged commits that replace the other files. The kernel releases
// the lock when the process ends.
class DirectoryLock {
 public:
  // Takes the lock without waiting. The description is
  // "database is locked by another process" when another open file description
  // holds it. A directory in which the lock file cannot be created or opened
  // for lack of permission yields no lock and no error. Such a process cannot
  // commit, because a commit creates files in the directory, but it can still
  // append to a table file it may write; only the change check of ADR-0022
  // guards that append.
  static LockOutcome Acquire(const std::string& directory);

  ~DirectoryLock();
  DirectoryLock(const DirectoryLock&) = delete;
  DirectoryLock& operator=(const DirectoryLock&) = delete;

 private:
  explicit DirectoryLock(int file_descriptor)
      : file_descriptor_(file_descriptor) {}

  int file_descriptor_;
};

}  // namespace strata::infrastructure

#endif  // STRATA_DIRECTORY_LOCK_H_
