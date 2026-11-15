#include "strata/directory_lock.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>

namespace strata::infrastructure {

namespace {

constexpr const char* kLockFileName = ".strata.lock";

LockOutcome Failure(const std::string& description) {
  return LockOutcome{nullptr, description};
}

bool IsPermissionFailure(int error_number) {
  return error_number == EACCES || error_number == EROFS;
}

// Creates the lock file when it is missing. A lock file that exists but cannot
// be opened for writing is opened read-only, which flock accepts.
int OpenLockFile(const std::string& path) {
  int file_descriptor =
      ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (file_descriptor < 0 && errno == EACCES) {
    int open_error = errno;
    file_descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (file_descriptor < 0) {
      errno = open_error;
    }
  }
  return file_descriptor;
}

}  // namespace

LockOutcome DirectoryLock::Acquire(const std::string& directory) {
  const std::string path = directory + "/" + kLockFileName;
  int file_descriptor = OpenLockFile(path);
  if (file_descriptor < 0) {
    if (IsPermissionFailure(errno)) {
      return LockOutcome{nullptr, std::nullopt};
    }
    return Failure(std::string("cannot create ") + kLockFileName + ": " +
                   std::strerror(errno));
  }
  if (::flock(file_descriptor, LOCK_EX | LOCK_NB) != 0) {
    int lock_error = errno;
    ::close(file_descriptor);
    if (lock_error == EWOULDBLOCK) {
      return Failure("database is locked by another process");
    }
    return Failure(std::string("cannot lock ") + kLockFileName + ": " +
                   std::strerror(lock_error));
  }
  return LockOutcome{
      std::shared_ptr<DirectoryLock>(new DirectoryLock(file_descriptor)),
      std::nullopt};
}

DirectoryLock::~DirectoryLock() { ::close(file_descriptor_); }

}  // namespace strata::infrastructure
