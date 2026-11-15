#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "strata/paged/paged_types.h"
#include "strata/paged/replacement_policy.h"
#include "test_harness.h"

// LruPolicy and TwoQPolicy.

namespace {

using strata::paged::FrameId;
using strata::paged::IReplacementPolicy;
using strata::paged::LruPolicy;
using strata::paged::MakeReplacementPolicy;
using strata::paged::PageId;
using strata::paged::ReplacementPolicyKind;
using strata::paged::TwoQPolicy;

// The evicted frame, or -1 when no frame is evictable.
int Evicted(IReplacementPolicy& policy) {
  std::optional<FrameId> victim = policy.Evict();
  return victim.has_value() ? static_cast<int>(*victim) : -1;
}

// Load a frame and make it evictable, as a pool does for a fetched page that
// has been released.
void LoadReleased(IReplacementPolicy& policy, FrameId frame, PageId page_id) {
  policy.RecordLoad(frame, page_id);
  policy.SetEvictable(frame, true);
}

// A page table in front of a policy, counting misses the way a buffer pool
// does: a hit pins and releases, a miss takes a free frame or evicts.
class PolicyDriver {
 public:
  PolicyDriver(ReplacementPolicyKind kind, std::size_t capacity)
      : policy_(MakeReplacementPolicy(kind, capacity)) {
    for (std::size_t frame = capacity; frame > 0; --frame) {
      free_frames_.push_back(static_cast<FrameId>(frame - 1));
    }
  }

  void Access(PageId page_id) {
    auto found = frame_of_page_.find(page_id);
    if (found != frame_of_page_.end()) {
      policy_->RecordAccess(found->second);
      policy_->SetEvictable(found->second, false);
      policy_->SetEvictable(found->second, true);
      return;
    }
    ++misses_;
    FrameId frame = 0;
    if (!free_frames_.empty()) {
      frame = free_frames_.back();
      free_frames_.pop_back();
    } else {
      std::optional<FrameId> victim = policy_->Evict();
      if (!victim.has_value()) {
        return;
      }
      frame = *victim;
      frame_of_page_.erase(page_of_frame_[frame]);
    }
    frame_of_page_[page_id] = frame;
    page_of_frame_[frame] = page_id;
    policy_->RecordLoad(frame, page_id);
    policy_->SetEvictable(frame, false);
    policy_->SetEvictable(frame, true);
  }

  int misses() const { return misses_; }

 private:
  std::unique_ptr<IReplacementPolicy> policy_;
  std::vector<FrameId> free_frames_;
  std::unordered_map<PageId, FrameId> frame_of_page_;
  std::unordered_map<FrameId, PageId> page_of_frame_;
  int misses_ = 0;
};

// Runs the access sequence of the scan-resistance test and returns the misses
// of each phase: first hot round, fillers, second hot round, scan, final hot
// round.
std::vector<int> RunScanWorkload(ReplacementPolicyKind kind) {
  PolicyDriver driver(kind, 8);
  std::vector<int> phase_misses;
  int previous = 0;
  const PageId hot_base = 1000;
  const PageId filler_base = 2000;
  const PageId scan_base = 3000;
  // First load of the hot set, then three more rounds that all hit.
  for (PageId page = 0; page < 4; ++page) {
    driver.Access(hot_base + page);
  }
  phase_misses.push_back(driver.misses() - previous);
  previous = driver.misses();
  for (int round = 0; round < 3; ++round) {
    for (PageId page = 0; page < 4; ++page) {
      driver.Access(hot_base + page);
    }
  }
  CHECK_EQ(driver.misses() - previous, 0);
  for (PageId page = 0; page < 8; ++page) {
    driver.Access(filler_base + page);
  }
  phase_misses.push_back(driver.misses() - previous);
  previous = driver.misses();
  for (PageId page = 0; page < 4; ++page) {
    driver.Access(hot_base + page);
  }
  phase_misses.push_back(driver.misses() - previous);
  previous = driver.misses();
  for (PageId page = 0; page < 40; ++page) {
    driver.Access(scan_base + page);
  }
  phase_misses.push_back(driver.misses() - previous);
  previous = driver.misses();
  for (PageId page = 0; page < 4; ++page) {
    driver.Access(hot_base + page);
  }
  phase_misses.push_back(driver.misses() - previous);
  return phase_misses;
}

}  // namespace

TEST(test_paged_lru_evicts_in_least_recently_used_order) {
  LruPolicy policy;
  for (FrameId frame = 0; frame < 4; ++frame) {
    LoadReleased(policy, frame, 100 + frame);
  }
  CHECK_EQ(Evicted(policy), 0);
  CHECK_EQ(Evicted(policy), 1);
}

TEST(test_paged_lru_respects_pins_access_and_remove) {
  LruPolicy policy;
  for (FrameId frame = 0; frame < 3; ++frame) {
    LoadReleased(policy, frame, 100 + frame);
  }
  policy.RecordAccess(0);
  policy.SetEvictable(1, false);
  CHECK_EQ(Evicted(policy), 2);
  CHECK_EQ(Evicted(policy), 0);
  policy.SetEvictable(1, true);
  CHECK_EQ(Evicted(policy), 1);
  CHECK_EQ(Evicted(policy), -1);
  policy.RecordLoad(5, 105);
  policy.Remove(5);
  CHECK_EQ(Evicted(policy), -1);
}

TEST(test_paged_twoq_promotes_pages_seen_in_ghost_queue) {
  // Capacity 8: Kin is 2 and Kout is 4.
  TwoQPolicy policy(8);
  for (FrameId frame = 0; frame < 6; ++frame) {
    LoadReleased(policy, frame, 100 + frame);
  }
  CHECK_EQ(Evicted(policy), 0);
  LoadReleased(policy, 0, 100);  // page 100 is in A1out: it goes to Am
  CHECK_EQ(Evicted(policy), 1);
  CHECK_EQ(Evicted(policy), 2);
  CHECK_EQ(Evicted(policy), 3);
  CHECK_EQ(Evicted(policy), 0);  // A1in is down to Kin, so Am is used
  LoadReleased(policy, 1, 101);  // page 101 is in A1out
  CHECK_EQ(Evicted(policy), 1);
}

TEST(test_paged_twoq_resists_scans_where_lru_does_not) {
  const std::vector<int> lru = RunScanWorkload(ReplacementPolicyKind::kLru);
  const std::vector<int> twoq = RunScanWorkload(ReplacementPolicyKind::kTwoQ);
  const std::vector<int> same_before_final = {4, 8, 4, 40};
  CHECK_EQ(lru.size(), static_cast<std::size_t>(5));
  CHECK_EQ(twoq.size(), static_cast<std::size_t>(5));
  if (lru.size() != 5 || twoq.size() != 5) {
    return;
  }
  CHECK(std::vector<int>(lru.begin(), lru.begin() + 4) == same_before_final);
  CHECK(std::vector<int>(twoq.begin(), twoq.begin() + 4) == same_before_final);
  CHECK_EQ(lru[4], 4);
  CHECK_EQ(twoq[4], 0);
}

TEST(test_paged_policies_handle_capacity_one_and_ghost_trimming) {
  // (a) Capacity 1.
  {
    LruPolicy lru;
    TwoQPolicy twoq(1);
    IReplacementPolicy* const policies[] = {&lru, &twoq};
    for (IReplacementPolicy* policy : policies) {
      LoadReleased(*policy, 0, 10);
      CHECK_EQ(Evicted(*policy), 0);
      CHECK_EQ(Evicted(*policy), -1);
      LoadReleased(*policy, 0, 10);
      CHECK_EQ(Evicted(*policy), 0);
      CHECK_EQ(Evicted(*policy), -1);
    }
  }
  // (b) Capacity 8 (Kin 2, Kout 4): A1out keeps the four newest page ids.
  TwoQPolicy policy(8);
  for (FrameId frame = 0; frame < 8; ++frame) {
    LoadReleased(policy, frame, frame);
  }
  for (FrameId cycle = 0; cycle < 5; ++cycle) {
    const int victim = Evicted(policy);
    CHECK_EQ(victim, static_cast<int>(cycle));
    LoadReleased(policy, static_cast<FrameId>(victim), 100 + cycle);
  }
  // A1out is now pages 1 to 4; page 0 was trimmed.
  int victim = Evicted(policy);
  CHECK_EQ(victim, 5);
  LoadReleased(policy, static_cast<FrameId>(victim), 4);  // in A1out: Am
  victim = Evicted(policy);
  CHECK_EQ(victim, 6);
  LoadReleased(policy, static_cast<FrameId>(victim), 0);  // trimmed: A1in
  std::vector<int> order;
  for (int next = Evicted(policy); next != -1; next = Evicted(policy)) {
    order.push_back(next);
  }
  const std::vector<int> expected = {7, 0, 1, 2, 3, 5, 4, 6};
  CHECK(order == expected);
}
