#pragma once

#include <array>
#include <cstdint>
#include <limits>

namespace sigap::acquisition {

class CooperativeScheduler final {
 public:
  using Callback = void (*)(void*, uint64_t) noexcept;
  static constexpr std::size_t kMaxJobs{16};

  bool add_job(uint64_t period_us, Callback callback, void* context,
               uint64_t first_due_us) noexcept {
    if (period_us == 0U || callback == nullptr || count_ == kMaxJobs) return false;
    jobs_[count_++] = Job{true, period_us, first_due_us, callback, context};
    return true;
  }

  void poll(uint64_t now_us) noexcept {
    if (has_last_ && now_us < last_now_us_) return;
    has_last_ = true;
    last_now_us_ = now_us;
    for (std::size_t i = 0; i < count_; ++i) {
      auto& job = jobs_[i];
      if (!job.enabled || now_us < job.next_due_us) continue;
      job.callback(job.context, now_us);
      advance_deadline(job, now_us);
    }
  }

  bool deadline_overflowed() const noexcept { return deadline_overflowed_; }

 private:
  struct Job {
    bool enabled{false};
    uint64_t period_us{0};
    uint64_t next_due_us{0};
    Callback callback{nullptr};
    void* context{nullptr};
  };
  std::array<Job, kMaxJobs> jobs_{};
  std::size_t count_{0};
  bool has_last_{false};
  uint64_t last_now_us_{0};
  bool deadline_overflowed_{false};

  void advance_deadline(Job& job, uint64_t now_us) noexcept {
    // Keep the original phase instead of using now + period. A late poll
    // skips missed periods and executes at most one callback per poll.
    const uint64_t late_us = now_us - job.next_due_us;
    const uint64_t periods_late = late_us / job.period_us;
    if (periods_late == std::numeric_limits<uint64_t>::max()) {
      job.enabled = false;
      deadline_overflowed_ = true;
      return;
    }

    const uint64_t periods_to_advance = periods_late + 1U;
    if (periods_to_advance >
        (std::numeric_limits<uint64_t>::max() - job.next_due_us) /
            job.period_us) {
      job.enabled = false;
      deadline_overflowed_ = true;
      return;
    }

    job.next_due_us += periods_to_advance * job.period_us;
  }
};

}  // namespace sigap::acquisition
