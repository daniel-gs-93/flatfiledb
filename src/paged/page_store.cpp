#include "strata/paged/page_store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

namespace strata::paged {

namespace {

std::string ErrnoText() { return std::strerror(errno); }

off_t OffsetOf(PageId page_id) {
  return static_cast<off_t>(page_id) * static_cast<off_t>(kPageSize);
}

}  // namespace

FilePageStore::FilePageStore(int file_descriptor, std::filesystem::path path,
                             PageId page_count)
    : file_descriptor_(file_descriptor),
      path_(std::move(path)),
      page_count_(page_count) {}

FilePageStore::FilePageStore(FilePageStore&& other) noexcept
    : file_descriptor_(other.file_descriptor_),
      path_(std::move(other.path_)),
      page_count_(other.page_count_) {
  other.file_descriptor_ = -1;
}

FilePageStore::~FilePageStore() {
  if (file_descriptor_ >= 0) {
    ::close(file_descriptor_);
  }
}

PageResult<FilePageStore> FilePageStore::Open(const std::filesystem::path& path,
                                              OpenMode mode) {
  int flags = O_RDWR | O_CLOEXEC;
  if (mode == OpenMode::kOpenOrCreate) {
    flags |= O_CREAT;
  }
  const int file_descriptor = ::open(path.c_str(), flags, 0644);
  if (file_descriptor < 0) {
    return MakeError(PageErrorCode::kIo,
                     "cannot open " + path.string() + ": " + ErrnoText());
  }
  struct stat file_status;
  if (::fstat(file_descriptor, &file_status) != 0) {
    const std::string reason = ErrnoText();
    ::close(file_descriptor);
    return MakeError(PageErrorCode::kIo,
                     "cannot inspect " + path.string() + ": " + reason);
  }
  const auto file_size = static_cast<std::uint64_t>(file_status.st_size);
  if (file_size % kPageSize != 0) {
    ::close(file_descriptor);
    return MakeError(PageErrorCode::kCorrupt,
                     path.string() + ": size " + std::to_string(file_size) +
                         " is not a multiple of " + std::to_string(kPageSize));
  }
  if (file_size / kPageSize >= kInvalidPageId) {
    ::close(file_descriptor);
    return MakeError(PageErrorCode::kCorrupt,
                     path.string() + ": too many pages for a 32-bit page id");
  }
  return FilePageStore(file_descriptor, path,
                       static_cast<PageId>(file_size / kPageSize));
}

Status FilePageStore::ReadPage(PageId page_id, std::uint8_t* destination) {
  if (page_id >= page_count_) {
    return MakeError(PageErrorCode::kNotFound,
                     "page " + std::to_string(page_id) +
                         " is out of range (page count " +
                         std::to_string(page_count_) + ")");
  }
  std::size_t done = 0;
  while (done < kPageSize) {
    const ssize_t count =
        ::pread(file_descriptor_, destination + done, kPageSize - done,
                OffsetOf(page_id) + static_cast<off_t>(done));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return MakeError(PageErrorCode::kIo,
                       "cannot read page " + std::to_string(page_id) + " of " +
                           path_.string() + ": " + ErrnoText());
    }
    if (count == 0) {
      return MakeError(PageErrorCode::kIo,
                       "cannot read page " + std::to_string(page_id) + " of " +
                           path_.string() + ": unexpected end of file");
    }
    done += static_cast<std::size_t>(count);
  }
  return std::nullopt;
}

Status FilePageStore::WritePage(PageId page_id, const std::uint8_t* source) {
  if (page_id >= page_count_) {
    return MakeError(PageErrorCode::kNotFound,
                     "page " + std::to_string(page_id) +
                         " is out of range (page count " +
                         std::to_string(page_count_) + ")");
  }
  std::size_t done = 0;
  while (done < kPageSize) {
    const ssize_t count =
        ::pwrite(file_descriptor_, source + done, kPageSize - done,
                 OffsetOf(page_id) + static_cast<off_t>(done));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return MakeError(PageErrorCode::kIo,
                       "cannot write page " + std::to_string(page_id) + " of " +
                           path_.string() + ": " + ErrnoText());
    }
    done += static_cast<std::size_t>(count);
  }
  return std::nullopt;
}

PageResult<PageId> FilePageStore::AllocatePage() {
  if (page_count_ + 1 >= kInvalidPageId) {
    return MakeError(PageErrorCode::kNoSpace,
                     path_.string() + ": the page numbers are exhausted");
  }
  const std::array<std::uint8_t, kPageSize> zeros{};
  std::size_t done = 0;
  while (done < kPageSize) {
    const ssize_t count =
        ::pwrite(file_descriptor_, zeros.data() + done, kPageSize - done,
                 OffsetOf(page_count_) + static_cast<off_t>(done));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return MakeError(PageErrorCode::kIo,
                       "cannot extend " + path_.string() + ": " + ErrnoText());
    }
    done += static_cast<std::size_t>(count);
  }
  return page_count_++;
}

Status FilePageStore::Sync() {
#ifdef __APPLE__
  // fsync on macOS may leave data in the drive's cache; F_FULLFSYNC does not.
  // Some file systems do not support it, so fsync is the fallback.
  if (::fcntl(file_descriptor_, F_FULLFSYNC) == 0) {
    return std::nullopt;
  }
#endif
  if (::fsync(file_descriptor_) == 0) {
    return std::nullopt;
  }
  return MakeError(PageErrorCode::kIo,
                   "cannot sync " + path_.string() + ": " + ErrnoText());
}

MemoryPageStore::MemoryPageStore(std::size_t page_count) {
  for (std::size_t i = 0; i < page_count; ++i) {
    pages_.emplace_back();
    pages_.back().fill(0);
  }
}

bool MemoryPageStore::ShouldFail(Operation operation) {
  const auto index = static_cast<std::size_t>(operation);
  ++call_counts_[index];
  if (failure_countdown_[index] == 0) {
    return false;
  }
  --failure_countdown_[index];
  return failure_countdown_[index] == 0;
}

Status MemoryPageStore::ReadPage(PageId page_id, std::uint8_t* destination) {
  if (ShouldFail(Operation::kRead)) {
    return MakeError(PageErrorCode::kIo, "injected read failure at page " +
                                             std::to_string(page_id));
  }
  if (page_id >= pages_.size()) {
    return MakeError(PageErrorCode::kNotFound,
                     "page " + std::to_string(page_id) +
                         " is out of range (page count " +
                         std::to_string(pages_.size()) + ")");
  }
  std::memcpy(destination, pages_[page_id].data(), kPageSize);
  return std::nullopt;
}

Status MemoryPageStore::WritePage(PageId page_id, const std::uint8_t* source) {
  if (ShouldFail(Operation::kWrite)) {
    return MakeError(PageErrorCode::kIo, "injected write failure at page " +
                                             std::to_string(page_id));
  }
  if (page_id >= pages_.size()) {
    return MakeError(PageErrorCode::kNotFound,
                     "page " + std::to_string(page_id) +
                         " is out of range (page count " +
                         std::to_string(pages_.size()) + ")");
  }
  std::memcpy(pages_[page_id].data(), source, kPageSize);
  return std::nullopt;
}

PageResult<PageId> MemoryPageStore::AllocatePage() {
  if (ShouldFail(Operation::kAllocate)) {
    return MakeError(PageErrorCode::kIo, "injected allocate failure");
  }
  pages_.emplace_back();
  pages_.back().fill(0);
  return static_cast<PageId>(pages_.size() - 1);
}

Status MemoryPageStore::Sync() {
  if (ShouldFail(Operation::kSync)) {
    return MakeError(PageErrorCode::kIo, "injected sync failure");
  }
  return std::nullopt;
}

void MemoryPageStore::InjectFailure(Operation operation, int nth_call) {
  failure_countdown_[static_cast<std::size_t>(operation)] = nth_call;
}

void MemoryPageStore::ClearInjectedFailures() { failure_countdown_.fill(0); }

int MemoryPageStore::CallCount(Operation operation) const {
  return call_counts_[static_cast<std::size_t>(operation)];
}

std::uint8_t* MemoryPageStore::MutablePage(PageId page_id) {
  return pages_[page_id].data();
}

const std::uint8_t* MemoryPageStore::PageBytes(PageId page_id) const {
  return pages_[page_id].data();
}

}  // namespace strata::paged
