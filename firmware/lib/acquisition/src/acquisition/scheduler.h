#pragma once

#include <array>
#include <cstdint>

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
      job.next_due_us = now_us + job.period_us;
    }
  }

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
};

}  // namespace sigap::acquisition
