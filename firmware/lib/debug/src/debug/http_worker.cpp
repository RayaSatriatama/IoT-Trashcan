#include "debug/http_worker.h"
#include <algorithm>
#include "debug/codec.h"
#include "telemetry/sha256.h"
namespace sigap::debug {
namespace {
constexpr uint64_t kSecondUs = 1000000ULL;
constexpr uint64_t kCadenceUs = 5ULL * kSecondUs;
constexpr uint32_t kDeadlineMs = 5000U;
constexpr uint8_t kMaxAttempts = 3U;

bool copy_bounded(std::string_view in, char* out, size_t cap, size_t& size) noexcept {
  size = 0U;
  if (in.empty() || in.size() >= cap) return false;
  std::copy(in.begin(), in.end(), out);
  out[in.size()] = '\0';
  size = in.size();
  return true;
}
uint64_t after(uint64_t now, uint64_t delay) noexcept {
  return delay > UINT64_MAX - now ? UINT64_MAX : now + delay;
}
bool valid_endpoint(std::string_view endpoint) noexcept {
  constexpr std::string_view scheme = "https://";
  if (endpoint.size() <= scheme.size() ||
      endpoint.substr(0U, scheme.size()) != scheme ||
      endpoint.find_first_of("/?#", scheme.size()) == scheme.size()) return false;
  for (const unsigned char byte : endpoint) {
    if (byte <= 0x20U || byte == 0x7fU) return false;
  }
  return true;
}
bool ready(const DebugReadiness& r) noexcept {
  return r.wifi && r.time_valid && r.ca_ready && r.config_ready &&
      !r.control_active && !r.other_transport_active;
}
}  // namespace

DebugHttpWorker::DebugHttpWorker(DebugService& service, IDebugHttpTransport& transport,
                                 RuntimeIoGate& gate, std::string_view device) noexcept
    : service_(service), transport_(transport), gate_(gate) {
  if (copy_bounded(device, device_id_.data(), device_id_.size(), device_size_))
    cursor_.boot = service_.meta(0U).boot;
}
bool DebugHttpWorker::configure(std::string_view endpoint,
                                std::string_view token) noexcept {
  size_t endpoint_size = 0U, token_size = 0U;
  std::array<char, 193> endpoint_copy{};
  std::array<char, 129> token_copy{};
  if (!device_size_ || owns_transport_ ||
      !copy_bounded(endpoint, endpoint_copy.data(), endpoint_copy.size(), endpoint_size) ||
      !valid_endpoint({endpoint_copy.data(), endpoint_size}) ||
      !copy_bounded(token, token_copy.data(), token_copy.size(), token_size)) return false;
  const bool changed = !configured_ || endpoint_size_ != endpoint_size ||
      token_size_ != token_size ||
      !std::equal(endpoint_copy.begin(), endpoint_copy.begin() + endpoint_size, endpoint_.begin()) ||
      !std::equal(token_copy.begin(), token_copy.begin() + token_size, token_.begin());
  if (!changed) return true;
  endpoint_ = endpoint_copy;
  endpoint_size_ = endpoint_size;
  token_ = token_copy;
  token_size_ = token_size;
  configured_ = true;
  if (changed) {
    sticky_stop_ = false;
    attempts_ = 0U;
    retry_at_us_ = 0U;
  }
  status_ = sticky_stop_ ? DebugWorkerStatus::Stopped : DebugWorkerStatus::Idle;
  return true;
}
bool DebugHttpWorker::prepare(uint64_t now) noexcept {
  frozen_ = {};
  body_size_ = 0U;
  if (!service_.capture(cursor_, frozen_.events.size(), now, frozen_)) return false;
  for (size_t count = frozen_.count; count > 0U; --count) {
    frozen_.count = count;
    size_t length = 0U;
    if (encode_upload({device_id_.data(), device_size_}, frozen_, body_.data(),
                      body_.size(), length)) {
      body_size_ = length;
      break;
    }
  }
  if (!body_size_) return false;
  digest_ = telemetry_contract::sha256({body_.data(), body_size_});
  attempts_ = 0U;
  return true;
}
void DebugHttpWorker::fail_attempt(uint64_t now) noexcept {
  if (attempts_ >= kMaxAttempts) {
    const uint64_t lost = frozen_.count;
    service_.note_delivery_loss(lost);
    delivery_loss_ = lost > UINT64_MAX - delivery_loss_ ? UINT64_MAX : delivery_loss_ + lost;
    if (frozen_.count) cursor_.after = frozen_.events[frozen_.count - 1U].sequence;
    attempts_ = 0U;
    body_size_ = 0U;
    next_start_us_ = after(now, kCadenceUs);
    status_ = DebugWorkerStatus::DeliveryFailure;
    return;
  }
  // Only 5/10 seconds are reachable with three total attempts; both obey the 60s cap.
  const uint64_t delay = std::min<uint64_t>(5ULL << (attempts_ - 1U), 60ULL) * kSecondUs;
  retry_at_us_ = after(now, delay);
  status_ = DebugWorkerStatus::RetryScheduled;
}
void DebugHttpWorker::release_if_quiescent() noexcept {
  if (owns_transport_ && transport_.quiescent()) {
    gate_.release(IoOwner::DebugHttp);
    owns_transport_ = false;
  }
}
void DebugHttpWorker::cancel() noexcept {
  if (owns_transport_ && !canceling_) {
    transport_.cancel();
    canceling_ = true;
  }
}
void DebugHttpWorker::finish_attempt(uint64_t now) noexcept {
  if (completed_result_ != network::TransportResult::Response) {
    fail_attempt(now);
    return;
  }
  const int status = completed_.status_code;
  if (status != 200) {
    if (status == 408 || status == 429 || (status >= 500 && status <= 599)) {
      fail_attempt(now);
    } else {
      sticky_stop_ = true;
      status_ = DebugWorkerStatus::Stopped;
    }
    return;
  }
  UploadAck ack{};
  if (completed_.truncated || completed_.ack_size > completed_.ack_body.size() ||
      !decode_upload_ack({completed_.ack_body.data(), completed_.ack_size}, ack) ||
      !matches_upload_ack(ack, {device_id_.data(), device_size_}, frozen_, digest_)) {
    sticky_stop_ = true;
    status_ = DebugWorkerStatus::Stopped;
    return;
  }
  cursor_.after = frozen_.events[frozen_.count - 1U].sequence;
  attempts_ = 0U;
  body_size_ = 0U;
  next_start_us_ = after(now, kCadenceUs);
  status_ = DebugWorkerStatus::Delivered;
}
void DebugHttpWorker::poll_active(uint64_t now, const DebugReadiness& r) noexcept {
  if (canceling_ || !ready(r)) {
    cancel();
    release_if_quiescent();
    if (!owns_transport_) {
      canceling_ = false;
      response_ready_ = false;
      fail_attempt(now);
    }
    return;
  }
  if (!response_ready_) {
    if (now >= deadline_us_) {
      cancel();
      release_if_quiescent();
      if (!owns_transport_) {
        canceling_ = false;
        fail_attempt(now);
      }
      return;
    }
    completed_ = {};
    completed_result_ = transport_.poll(completed_);
    if (completed_result_ == network::TransportResult::Pending) return;
    response_ready_ = true;
  }
  if (!transport_.quiescent()) return;
  release_if_quiescent();
  canceling_ = false;
  response_ready_ = false;
  finish_attempt(now);
}
void DebugHttpWorker::poll(uint64_t now, const DebugReadiness& r) noexcept {
  if (!configured_) {
    status_ = DebugWorkerStatus::Unconfigured;
    return;
  }
  if (sticky_stop_) {
    status_ = DebugWorkerStatus::Stopped;
    return;
  }
  if (owns_transport_) {
    poll_active(now, r);
    return;
  }
  if (!ready(r)) {
    status_ = DebugWorkerStatus::Unavailable;
    return;
  }
  if (now < retry_at_us_ || now < next_start_us_) return;
  if (!body_size_ && !prepare(now)) {
    status_ = DebugWorkerStatus::Idle;
    return;
  }
  if (!gate_.try_acquire(IoOwner::DebugHttp)) {
    status_ = DebugWorkerStatus::Unavailable;
    return;
  }
  owns_transport_ = true;
  ++attempts_;
  deadline_us_ = after(now, kDeadlineMs * 1000ULL);
  const auto result = transport_.start({body_.data(), body_size_},
                                      {token_.data(), token_size_}, kDeadlineMs);
  status_ = DebugWorkerStatus::Waiting;
  if (result == network::TransportResult::Pending) return;
  completed_ = {};
  completed_result_ = result == network::TransportResult::Response
      ? transport_.poll(completed_) : result;
  response_ready_ = completed_result_ != network::TransportResult::Pending;
  if (response_ready_ && completed_result_ != network::TransportResult::Response &&
      transport_.quiescent()) {
    release_if_quiescent();
    response_ready_ = false;
    fail_attempt(now);
  }
}
} // namespace sigap::debug
