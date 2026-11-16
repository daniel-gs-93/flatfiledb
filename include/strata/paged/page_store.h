#ifndef STRATA_PAGED_PAGE_STORE_H_
#define STRATA_PAGED_PAGE_STORE_H_

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>

#include "strata/paged/paged_types.h"

// The lowest layer of the paged engine: numbered pages of kPageSize bytes on a
// storage medium (design document 2.2, ADR-0030). The interface is owned by
// this layer; FilePageStore is the real implementation and MemoryPageStore a
// test double that can be told to fail.
//
// An IPageStore is not thread-safe. BufferManager calls it with its pool
// mutex held, so only one thread uses it at a time.

namespace strata::paged {

class IPageStore {
 public:
  virtual ~IPageStore() = default;

  // Copies page page_id into destination, which must have kPageSize bytes.
  // kNotFound: page_id is not below PageCount(). kIo: the medium failed.
  virtual Status ReadPage(PageId page_id, std::uint8_t* destination) = 0;

  // Copies kPageSize bytes from source over page page_id. kNotFound: page_id
  // is not below PageCount(). kIo: the medium failed.
  virtual Status WritePage(PageId page_id, const std::uint8_t* source) = 0;

  // Writes a page of zeros after the last whole page and returns its number
  // (the previous PageCount()).
  virtual PageResult<PageId> AllocatePage() = 0;

  virtual PageId PageCount() const = 0;

  // Hands everything written so far to the storage device.
  virtual Status Sync() = 0;
};

enum class OpenMode { kOpenExisting, kOpenOrCreate };

// Pages in one file: page n is at byte offset n * kPageSize, so PageCount() is
// the number of whole pages. Bytes after the last whole page, which an
// interrupted AllocatePage leaves, are ignored when they are all zero.
// Reads and writes use pread and pwrite.
// Sync uses fcntl(F_FULLFSYNC) on Apple platforms, where fsync does not
// guarantee that data leaves the device's cache, and fsync elsewhere.
// A FilePageStore owns its file descriptor and closes it on destruction.
class FilePageStore : public IPageStore {
 public:
  // kIo: the file cannot be opened or inspected. kCorrupt: the file size is
  // not a multiple of kPageSize and a byte after the last whole page is not
  // zero.
  static PageResult<FilePageStore> Open(const std::filesystem::path& path,
                                        OpenMode mode);

  FilePageStore(FilePageStore&& other) noexcept;
  FilePageStore(const FilePageStore&) = delete;
  FilePageStore& operator=(const FilePageStore&) = delete;
  FilePageStore& operator=(FilePageStore&&) = delete;
  ~FilePageStore() override;

  Status ReadPage(PageId page_id, std::uint8_t* destination) override;
  Status WritePage(PageId page_id, const std::uint8_t* source) override;
  PageResult<PageId> AllocatePage() override;
  PageId PageCount() const override { return page_count_; }
  Status Sync() override;

 private:
  FilePageStore(int file_descriptor, std::filesystem::path path,
                PageId page_count);

  int file_descriptor_;
  std::filesystem::path path_;
  PageId page_count_;
};

// Pages in memory. A test can make the n-th call of an operation fail once, to
// reach error paths that a real file does not reproduce, and can read or
// change the bytes of a page directly.
class MemoryPageStore : public IPageStore {
 public:
  enum class Operation { kRead = 0, kWrite = 1, kAllocate = 2, kSync = 3 };

  // A store with page_count pages of zeros.
  explicit MemoryPageStore(std::size_t page_count = 0);

  Status ReadPage(PageId page_id, std::uint8_t* destination) override;
  Status WritePage(PageId page_id, const std::uint8_t* source) override;
  PageResult<PageId> AllocatePage() override;
  PageId PageCount() const override {
    return static_cast<PageId>(pages_.size());
  }
  Status Sync() override;

  // The nth_call-th call of operation from now on fails with kIo, once.
  // nth_call 1 is the next call.
  void InjectFailure(Operation operation, int nth_call);
  void ClearInjectedFailures();

  // How many times each operation was called, failures included.
  int CallCount(Operation operation) const;

  // Direct access to a page; the pointer stays valid until the store is
  // destroyed. Precondition: page_id < PageCount().
  std::uint8_t* MutablePage(PageId page_id);
  const std::uint8_t* PageBytes(PageId page_id) const;

 private:
  // True when this call of the operation is the one injected to fail.
  bool ShouldFail(Operation operation);

  // A deque keeps the address of every page stable while pages are appended.
  std::deque<std::array<std::uint8_t, kPageSize>> pages_;
  std::array<int, 4> failure_countdown_{};
  std::array<int, 4> call_counts_{};
};

}  // namespace strata::paged

#endif  // STRATA_PAGED_PAGE_STORE_H_
