#ifndef FLATFILEDB_PAGED_REPLACEMENT_POLICY_H_
#define FLATFILEDB_PAGED_REPLACEMENT_POLICY_H_

#include <cstddef>
#include <list>
#include <memory>
#include <optional>
#include <unordered_map>

#include "flatfiledb/paged/paged_types.h"

// Which frame the buffer pool gives up when it is full (design document 2.7).
// Pure data structures: no I/O and no locks. BufferManager calls every
// operation while it holds the pool mutex, so an implementation is used by
// one thread at a time.

namespace flatfiledb::paged {

class IReplacementPolicy {
 public:
  virtual ~IReplacementPolicy() = default;

  // The frame now holds page_id. The frame is not evictable until
  // SetEvictable(frame, true); a frame that was already tracked is replaced.
  virtual void RecordLoad(FrameId frame, PageId page_id) = 0;

  // The frame's page was used again.
  virtual void RecordAccess(FrameId frame) = 0;

  // Evictable means no pin holds the frame. Setting it to true also counts as
  // one access. A frame the policy does not track is ignored.
  virtual void SetEvictable(FrameId frame, bool is_evictable) = 0;

  // Chooses an evictable frame, stops tracking it, and returns it; no value
  // when no frame is evictable. The caller reloads the frame with RecordLoad.
  virtual std::optional<FrameId> Evict() = 0;

  // Stops tracking the frame.
  virtual void Remove(FrameId frame) = 0;
};

// Least recently used. A list of the evictable frames, least recently used at
// the front, plus a map from frame to list position: RecordAccess,
// SetEvictable, Evict and Remove are O(1).
class LruPolicy : public IReplacementPolicy {
 public:
  void RecordLoad(FrameId frame, PageId page_id) override;
  void RecordAccess(FrameId frame) override;
  void SetEvictable(FrameId frame, bool is_evictable) override;
  std::optional<FrameId> Evict() override;
  void Remove(FrameId frame) override;

 private:
  std::list<FrameId> evictable_order_;
  std::unordered_map<FrameId, std::list<FrameId>::iterator> position_of_frame_;
};

// A simplified 2Q for a pool of `capacity` frames: A1in is a FIFO of frames
// whose page was loaded for the first time, Am is an LRU of frames whose page
// was seen again, and A1out is a FIFO of page ids (no frames) of pages
// recently evicted from A1in. Kin = max(1, capacity / 4) is the A1in size
// above which Evict prefers A1in; Kout = max(1, capacity / 2) bounds A1out.
// RecordLoad, RecordAccess and SetEvictable are O(1); Evict is O(capacity) in
// the worst case, when many frames at the head of a queue are pinned.
class TwoQPolicy : public IReplacementPolicy {
 public:
  explicit TwoQPolicy(std::size_t capacity);

  void RecordLoad(FrameId frame, PageId page_id) override;
  void RecordAccess(FrameId frame) override;
  void SetEvictable(FrameId frame, bool is_evictable) override;
  std::optional<FrameId> Evict() override;
  void Remove(FrameId frame) override;

 private:
  struct Entry {
    bool is_in_am;
    bool is_evictable;
    PageId page_id;
    std::list<FrameId>::iterator position;
  };

  // Removes the frame from its queue and from entries_, without touching
  // A1out.
  void Detach(FrameId frame);
  std::optional<FrameId> FirstEvictable(const std::list<FrameId>& queue) const;

  std::size_t a1in_limit_;
  std::size_t a1out_limit_;
  std::list<FrameId> a1in_;
  std::list<FrameId> am_;
  std::unordered_map<FrameId, Entry> entries_;
  std::size_t am_evictable_count_ = 0;
  std::list<PageId> a1out_;
  std::unordered_map<PageId, std::list<PageId>::iterator> a1out_position_;
};

// The caller owns the returned policy. capacity is the number of frames.
std::unique_ptr<IReplacementPolicy> MakeReplacementPolicy(
    ReplacementPolicyKind kind, std::size_t capacity);

}  // namespace flatfiledb::paged

#endif  // FLATFILEDB_PAGED_REPLACEMENT_POLICY_H_
