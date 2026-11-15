#include "strata/paged/replacement_policy.h"

#include <algorithm>
#include <list>
#include <memory>

namespace strata::paged {

void LruPolicy::RecordLoad(FrameId frame, PageId /*page_id*/) { Remove(frame); }

void LruPolicy::RecordAccess(FrameId frame) {
  auto found = position_of_frame_.find(frame);
  if (found == position_of_frame_.end()) {
    return;
  }
  evictable_order_.splice(evictable_order_.end(), evictable_order_,
                          found->second);
}

void LruPolicy::SetEvictable(FrameId frame, bool is_evictable) {
  auto found = position_of_frame_.find(frame);
  if (!is_evictable) {
    if (found != position_of_frame_.end()) {
      evictable_order_.erase(found->second);
      position_of_frame_.erase(found);
    }
    return;
  }
  if (found != position_of_frame_.end()) {
    evictable_order_.splice(evictable_order_.end(), evictable_order_,
                            found->second);
    return;
  }
  evictable_order_.push_back(frame);
  position_of_frame_.emplace(frame, std::prev(evictable_order_.end()));
}

std::optional<FrameId> LruPolicy::Evict() {
  if (evictable_order_.empty()) {
    return std::nullopt;
  }
  const FrameId victim = evictable_order_.front();
  evictable_order_.pop_front();
  position_of_frame_.erase(victim);
  return victim;
}

void LruPolicy::Remove(FrameId frame) {
  auto found = position_of_frame_.find(frame);
  if (found == position_of_frame_.end()) {
    return;
  }
  evictable_order_.erase(found->second);
  position_of_frame_.erase(found);
}

TwoQPolicy::TwoQPolicy(std::size_t capacity)
    : a1in_limit_(std::max<std::size_t>(1, capacity / 4)),
      a1out_limit_(std::max<std::size_t>(1, capacity / 2)) {}

void TwoQPolicy::Detach(FrameId frame) {
  auto found = entries_.find(frame);
  if (found == entries_.end()) {
    return;
  }
  const Entry& entry = found->second;
  if (entry.is_in_am) {
    if (entry.is_evictable) {
      --am_evictable_count_;
    }
    am_.erase(entry.position);
  } else {
    a1in_.erase(entry.position);
  }
  entries_.erase(found);
}

void TwoQPolicy::RecordLoad(FrameId frame, PageId page_id) {
  Detach(frame);
  Entry entry{false, false, page_id, {}};
  auto ghost = a1out_position_.find(page_id);
  if (ghost != a1out_position_.end()) {
    a1out_.erase(ghost->second);
    a1out_position_.erase(ghost);
    am_.push_back(frame);
    entry.is_in_am = true;
    entry.position = std::prev(am_.end());
  } else {
    a1in_.push_back(frame);
    entry.position = std::prev(a1in_.end());
  }
  entries_.emplace(frame, entry);
  // Truncating after the lookup keeps the page that was just requested from
  // being pushed out of A1out by the eviction that made room for it.
  while (a1out_.size() > a1out_limit_) {
    a1out_position_.erase(a1out_.front());
    a1out_.pop_front();
  }
}

void TwoQPolicy::RecordAccess(FrameId frame) {
  auto found = entries_.find(frame);
  if (found == entries_.end()) {
    return;
  }
  Entry& entry = found->second;
  // A second reference soon after the load is treated as part of the same
  // burst, so a frame in A1in does not move.
  if (entry.is_in_am) {
    am_.splice(am_.end(), am_, entry.position);
  }
}

void TwoQPolicy::SetEvictable(FrameId frame, bool is_evictable) {
  auto found = entries_.find(frame);
  if (found == entries_.end()) {
    return;
  }
  Entry& entry = found->second;
  if (entry.is_evictable != is_evictable) {
    entry.is_evictable = is_evictable;
    if (entry.is_in_am) {
      if (is_evictable) {
        ++am_evictable_count_;
      } else {
        --am_evictable_count_;
      }
    }
  }
  if (is_evictable) {
    RecordAccess(frame);
  }
}

std::optional<FrameId> TwoQPolicy::FirstEvictable(
    const std::list<FrameId>& queue) const {
  for (FrameId frame : queue) {
    if (entries_.at(frame).is_evictable) {
      return frame;
    }
  }
  return std::nullopt;
}

std::optional<FrameId> TwoQPolicy::Evict() {
  const bool is_preferring_a1in =
      a1in_.size() > a1in_limit_ || am_evictable_count_ == 0;
  std::optional<FrameId> victim;
  if (is_preferring_a1in) {
    victim = FirstEvictable(a1in_);
    if (!victim.has_value()) {
      victim = FirstEvictable(am_);
    }
  } else {
    victim = FirstEvictable(am_);
    if (!victim.has_value()) {
      victim = FirstEvictable(a1in_);
    }
  }
  if (!victim.has_value()) {
    return std::nullopt;
  }
  const Entry entry = entries_.at(*victim);
  if (!entry.is_in_am) {
    // The page id stays in A1out so a quick reload is recognised as reuse.
    // No truncation here; RecordLoad truncates after its lookup.
    auto stale_ghost = a1out_position_.find(entry.page_id);
    if (stale_ghost != a1out_position_.end()) {
      a1out_.erase(stale_ghost->second);
      a1out_position_.erase(stale_ghost);
    }
    a1out_.push_back(entry.page_id);
    a1out_position_[entry.page_id] = std::prev(a1out_.end());
  }
  Detach(*victim);
  return victim;
}

void TwoQPolicy::Remove(FrameId frame) { Detach(frame); }

std::unique_ptr<IReplacementPolicy> MakeReplacementPolicy(
    ReplacementPolicyKind kind, std::size_t capacity) {
  switch (kind) {
    case ReplacementPolicyKind::kLru:
      return std::make_unique<LruPolicy>();
    case ReplacementPolicyKind::kTwoQ:
      return std::make_unique<TwoQPolicy>(capacity);
  }
  return std::make_unique<LruPolicy>();
}

}  // namespace strata::paged
