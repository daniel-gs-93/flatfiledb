#include "strata/paged/buffer_manager.h"

#include <cassert>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "strata/paged/byte_codec.h"
#include "strata/paged/slotted_page.h"

namespace strata::paged {

namespace {

constexpr std::size_t kMinFrames = 4;

}  // namespace

PageGuard::PageGuard(BufferManager* buffer, FrameId frame, PageId page_id,
                     std::shared_lock<std::shared_mutex> latch,
                     std::uint8_t* data)
    : buffer_(buffer),
      frame_(frame),
      page_id_(page_id),
      mode_(LatchMode::kShared),
      data_(data),
      shared_latch_(std::move(latch)) {}

PageGuard::PageGuard(BufferManager* buffer, FrameId frame, PageId page_id,
                     std::unique_lock<std::shared_mutex> latch,
                     std::uint8_t* data)
    : buffer_(buffer),
      frame_(frame),
      page_id_(page_id),
      mode_(LatchMode::kExclusive),
      data_(data),
      exclusive_latch_(std::move(latch)) {}

PageGuard::PageGuard(PageGuard&& other) noexcept
    : buffer_(other.buffer_),
      frame_(other.frame_),
      page_id_(other.page_id_),
      mode_(other.mode_),
      data_(other.data_),
      is_dirty_(other.is_dirty_),
      shared_latch_(std::move(other.shared_latch_)),
      exclusive_latch_(std::move(other.exclusive_latch_)) {
  other.buffer_ = nullptr;
  other.data_ = nullptr;
  other.is_dirty_ = false;
}

PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  Release();
  buffer_ = other.buffer_;
  frame_ = other.frame_;
  page_id_ = other.page_id_;
  mode_ = other.mode_;
  data_ = other.data_;
  is_dirty_ = other.is_dirty_;
  shared_latch_ = std::move(other.shared_latch_);
  exclusive_latch_ = std::move(other.exclusive_latch_);
  other.buffer_ = nullptr;
  other.data_ = nullptr;
  other.is_dirty_ = false;
  return *this;
}

PageGuard::~PageGuard() { Release(); }

std::uint8_t* PageGuard::mutable_data() const {
  assert(mode_ == LatchMode::kExclusive);
  return data_;
}

void PageGuard::MarkDirty() {
  assert(mode_ == LatchMode::kExclusive);
  is_dirty_ = true;
}

void PageGuard::Release() {
  if (buffer_ == nullptr) {
    return;
  }
  // The latch goes first, then the pin: a frame whose pin count reaches 0 may
  // be evicted, and an evicted frame must have no latch holder.
  if (shared_latch_.owns_lock()) {
    shared_latch_.unlock();
  }
  if (exclusive_latch_.owns_lock()) {
    exclusive_latch_.unlock();
  }
  BufferManager* const buffer = buffer_;
  const bool is_dirty = is_dirty_;
  buffer_ = nullptr;
  data_ = nullptr;
  is_dirty_ = false;
  buffer->ReleasePin(frame_, is_dirty);
}

void BufferManager::AlignedFrameDeleter::operator()(
    std::uint8_t* frames) const {
  ::operator delete(frames, std::align_val_t{kPageSize});
}

PageResult<std::unique_ptr<BufferManager>> BufferManager::Create(
    IPageStore& store, std::size_t frames, ReplacementPolicyKind policy) {
  if (frames < kMinFrames) {
    return MakeError(PageErrorCode::kInvalidArgument,
                     "a buffer pool needs at least " +
                         std::to_string(kMinFrames) + " frames, got " +
                         std::to_string(frames));
  }
  void* const memory = ::operator new(
      frames * kPageSize, std::align_val_t{kPageSize}, std::nothrow);
  if (memory == nullptr) {
    return MakeError(PageErrorCode::kNoSpace,
                     "cannot allocate " + std::to_string(frames * kPageSize) +
                         " bytes for the frames");
  }
  std::memset(memory, 0, frames * kPageSize);
  std::unique_ptr<std::uint8_t, AlignedFrameDeleter> frame_memory(
      static_cast<std::uint8_t*>(memory));
  std::unique_ptr<FrameLatch[]> latches(new (std::nothrow) FrameLatch[frames]);
  if (latches == nullptr) {
    return MakeError(PageErrorCode::kNoSpace,
                     "cannot allocate the frame latches");
  }
  return std::unique_ptr<BufferManager>(new BufferManager(
      store, frames, policy, std::move(frame_memory), std::move(latches)));
}

BufferManager::BufferManager(
    IPageStore& store, std::size_t frame_count,
    ReplacementPolicyKind policy_kind,
    std::unique_ptr<std::uint8_t, AlignedFrameDeleter> frames,
    std::unique_ptr<FrameLatch[]> latches)
    : store_(store),
      frame_count_(frame_count),
      frames_(std::move(frames)),
      latches_(std::move(latches)),
      metas_(frame_count),
      policy_(MakeReplacementPolicy(policy_kind, frame_count)) {
  free_frames_.reserve(frame_count);
  // Frame 0 is taken first.
  for (std::size_t frame = frame_count; frame > 0; --frame) {
    free_frames_.push_back(static_cast<FrameId>(frame - 1));
  }
}

BufferManager::~BufferManager() = default;

std::uint8_t* BufferManager::FrameData(FrameId frame) const {
  return frames_.get() + static_cast<std::size_t>(frame) * kPageSize;
}

PageResult<FrameId> BufferManager::AcquireFrame() {
  if (!free_frames_.empty()) {
    const FrameId frame = free_frames_.back();
    free_frames_.pop_back();
    return frame;
  }
  const std::optional<FrameId> victim = policy_->Evict();
  if (!victim.has_value()) {
    return MakeError(
        PageErrorCode::kNoFrame,
        "all " + std::to_string(frame_count_) + " frames are pinned");
  }
  FrameMeta& meta = metas_[*victim];
  if (meta.is_dirty) {
    SlottedPage::SealChecksum(FrameData(*victim));
    if (Status status = store_.WritePage(meta.page_id, FrameData(*victim))) {
      // The write failed: the page stays in the pool, dirty, and is
      // evictable again.
      policy_->RecordLoad(*victim, meta.page_id);
      policy_->SetEvictable(*victim, true);
      return *status;
    }
    meta.is_dirty = false;
    ++stats_.dirty_writebacks;
    ++stats_.page_writes;
  }
  page_table_.erase(meta.page_id);
  meta = FrameMeta();
  ++stats_.evictions;
  return *victim;
}

PageResult<FrameId> BufferManager::PinPage(PageId page_id) {
  auto found = page_table_.find(page_id);
  if (found != page_table_.end()) {
    const FrameId frame = found->second;
    ++stats_.hits;
    ++metas_[frame].pin_count;
    policy_->RecordAccess(frame);
    policy_->SetEvictable(frame, false);
    return frame;
  }
  ++stats_.misses;
  PageResult<FrameId> acquired = AcquireFrame();
  if (!acquired.ok()) {
    return acquired.error();
  }
  const FrameId frame = acquired.value();
  if (Status status = store_.ReadPage(page_id, FrameData(frame))) {
    free_frames_.push_back(frame);
    return *status;
  }
  ++stats_.page_reads;
  if (Status status = SlottedPage::VerifyHeader(FrameData(frame), page_id)) {
    free_frames_.push_back(frame);
    return *status;
  }
  page_table_[page_id] = frame;
  metas_[frame] = FrameMeta{page_id, 1, false};
  policy_->RecordLoad(frame, page_id);
  policy_->SetEvictable(frame, false);
  return frame;
}

void BufferManager::UnpinLocked(FrameId frame) {
  FrameMeta& meta = metas_[frame];
  assert(meta.pin_count > 0);
  --meta.pin_count;
  if (meta.pin_count == 0) {
    policy_->SetEvictable(frame, true);
  }
}

void BufferManager::ReleasePin(FrameId frame, bool is_dirty) {
  std::lock_guard<std::mutex> pool_lock(pool_mutex_);
  if (is_dirty) {
    metas_[frame].is_dirty = true;
  }
  UnpinLocked(frame);
}

PageResult<PageGuard> BufferManager::FetchPage(PageId page_id, LatchMode mode) {
  FrameId frame = 0;
  {
    std::lock_guard<std::mutex> pool_lock(pool_mutex_);
    PageResult<FrameId> pinned = PinPage(page_id);
    if (!pinned.ok()) {
      return pinned.error();
    }
    frame = pinned.value();
  }
  // The pin keeps the frame from being evicted while this thread waits for
  // the latch, and the pool mutex is not held while it waits.
  if (mode == LatchMode::kShared) {
    std::shared_lock<std::shared_mutex> latch(latches_[frame].mutex);
    return PageGuard(this, frame, page_id, std::move(latch), FrameData(frame));
  }
  std::unique_lock<std::shared_mutex> latch(latches_[frame].mutex);
  return PageGuard(this, frame, page_id, std::move(latch), FrameData(frame));
}

PageResult<PageGuard> BufferManager::NewPage() {
  FrameId frame = 0;
  PageId page_id = kInvalidPageId;
  {
    std::lock_guard<std::mutex> pool_lock(pool_mutex_);
    PageResult<FrameId> acquired = AcquireFrame();
    if (!acquired.ok()) {
      return acquired.error();
    }
    frame = acquired.value();
    PageResult<PageId> allocated = store_.AllocatePage();
    if (!allocated.ok()) {
      free_frames_.push_back(frame);
      return allocated.error();
    }
    page_id = allocated.value();
    std::memset(FrameData(frame), 0, kPageSize);
    WriteLe32(FrameData(frame) + layout::kPageIdField, page_id);
    page_table_[page_id] = frame;
    metas_[frame] = FrameMeta{page_id, 1, true};
    policy_->RecordLoad(frame, page_id);
    policy_->SetEvictable(frame, false);
  }
  std::unique_lock<std::shared_mutex> latch(latches_[frame].mutex);
  return PageGuard(this, frame, page_id, std::move(latch), FrameData(frame));
}

Status BufferManager::WriteBackAll() {
  std::vector<FrameId> dirty_frames;
  {
    std::lock_guard<std::mutex> pool_lock(pool_mutex_);
    for (std::size_t frame = 0; frame < frame_count_; ++frame) {
      if (metas_[frame].page_id != kInvalidPageId && metas_[frame].is_dirty) {
        dirty_frames.push_back(static_cast<FrameId>(frame));
      }
    }
  }
  for (const FrameId frame : dirty_frames) {
    {
      std::lock_guard<std::mutex> pool_lock(pool_mutex_);
      FrameMeta& meta = metas_[frame];
      if (meta.page_id == kInvalidPageId) {
        continue;
      }
      ++meta.pin_count;
      policy_->SetEvictable(frame, false);
    }
    // Shared latch: no writer changes the page while it is written out. The
    // checksum bytes are written under it too; readers never read them.
    std::shared_lock<std::shared_mutex> latch(latches_[frame].mutex);
    Status failure;
    {
      std::lock_guard<std::mutex> pool_lock(pool_mutex_);
      FrameMeta& meta = metas_[frame];
      if (meta.is_dirty) {
        SlottedPage::SealChecksum(FrameData(frame));
        failure = store_.WritePage(meta.page_id, FrameData(frame));
        if (!failure.has_value()) {
          meta.is_dirty = false;
          ++stats_.page_writes;
        }
      }
      UnpinLocked(frame);
    }
    if (failure.has_value()) {
      return failure;
    }
  }
  return std::nullopt;
}

Status BufferManager::FlushAll() {
  if (Status failure = WriteBackAll()) {
    return failure;
  }
  std::lock_guard<std::mutex> pool_lock(pool_mutex_);
  return store_.Sync();
}

BufferStats BufferManager::Stats() const {
  std::lock_guard<std::mutex> pool_lock(pool_mutex_);
  return stats_;
}

void BufferManager::ResetStats() {
  std::lock_guard<std::mutex> pool_lock(pool_mutex_);
  stats_ = BufferStats();
}

std::size_t BufferManager::PinnedFrameCount() const {
  std::lock_guard<std::mutex> pool_lock(pool_mutex_);
  std::size_t count = 0;
  for (const FrameMeta& meta : metas_) {
    if (meta.pin_count > 0) {
      ++count;
    }
  }
  return count;
}

PageId BufferManager::PageCount() const {
  std::lock_guard<std::mutex> pool_lock(pool_mutex_);
  return store_.PageCount();
}

const void* BufferManager::FrameLatchAddress(FrameId frame) const {
  return &latches_[frame];
}

}  // namespace strata::paged
