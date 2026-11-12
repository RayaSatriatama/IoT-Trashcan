#include <string>
#include <algorithm>
#include <vector>
#include <type_traits>
#include <unity.h>
#include "debug/http_worker.h"
#include "debug/service.cpp"
#include "debug/codec.cpp"
#include "telemetry/sha256.h"
namespace {
using namespace sigap;
class Lock final : public debug::IDebugLock {
 public:
  void lock() noexcept override {}
  void unlock() noexcept override {}
};
class Transport final : public debug::IDebugHttpTransport {
 public:
  network::TransportResult start(std::string_view b, std::string_view t,
                                 uint32_t timeout) noexcept override {
    borrowed_body = b;
    borrowed_token = t;
    body.assign(b);
    token.assign(t);
    bodies.emplace_back(b);
    tokens.emplace_back(t);
    timeout_ms = timeout;
    ++starts;
    quiet = quiet_on_start;
    return start_result;
  }
  network::TransportResult poll(debug::DebugHttpResult& out) noexcept override {
    ++polls;
    if (!respond) return network::TransportResult::Pending;
    out.status_code = http_status;
    out.truncated = truncated;
    out.ack_size = reported_size ? reported_size : ack.size();
    std::copy_n(ack.begin(), std::min(ack.size(), out.ack_body.size()),
                out.ack_body.begin());
    return poll_result;
  }
  void cancel() noexcept override { cancel_requested = true; ++cancels; }
  bool quiescent() const noexcept override { return quiet; }

  std::string body, token, ack;
  std::vector<std::string> bodies, tokens;
  std::string_view borrowed_body, borrowed_token;
  network::TransportResult start_result{network::TransportResult::Pending};
  network::TransportResult poll_result{network::TransportResult::Response};
  uint32_t timeout_ms{};
  unsigned starts{}, polls{}, cancels{};
  int http_status{200};
  size_t reported_size{};
  bool cancel_requested{}, quiet{}, quiet_on_start{}, respond{}, truncated{};
};
acquisition::BootId boot() {
  acquisition::BootId result{};
  result.present = true;
  result.bytes[15] = 1U;
  return result;
}
std::string matching_ack(std::string_view body, const debug::BatchSnapshot& sent) {
  TEST_ASSERT_TRUE(sent.count > 0U && sent.count <= sent.events.size());
  static constexpr char hex[] = "0123456789abcdef";
  std::string digest_hex, boot_hex;
  for (const auto byte : telemetry_contract::sha256(body)) {
    digest_hex += hex[byte >> 4U];
    digest_hex += hex[byte & 15U];
  }
  for (const auto byte : sent.meta.boot.bytes) {
    boot_hex += hex[byte >> 4U];
    boot_hex += hex[byte & 15U];
  }
  return "{\"schema_version\":1,\"status\":\"DURABLE\",\"device_id\":\"device-1\","
      "\"boot_id\":\"" + boot_hex + "\",\"first_sequence\":\"" +
      std::to_string(sent.events[0].sequence) + "\",\"last_sequence\":\"" +
      std::to_string(sent.events[sent.count - 1U].sequence) + "\",\"stored_count\":" +
      std::to_string(sent.count) + ",\"deduplicated_count\":0,\"request_sha256\":\"" +
      digest_hex + "\"}";
}
constexpr uint64_t kSecond = 1000000ULL;
constexpr uint64_t kStart = 100U;

struct Fixture {
  Lock lock;
  debug::DebugService service{boot(), "TEST", debug::Level::Debug, lock};
  debug::RuntimeIoGate gate;
  Transport transport;
  debug::DebugHttpWorker worker{service, transport, gate, "device-1"};
  debug::DebugReadiness ready{true, true, true, true, false, false};

  Fixture() {
    TEST_ASSERT_TRUE(worker.configure("https://example.invalid/", "token"));
  }
  void emit(size_t count = 1U, uint64_t time = 10U) {
    debug::EventInput event{};
    event.monotonic_us = time;
    event.module = debug::Module::Runtime;
    event.event_code = 1;
    for (size_t i = 0; i < count; ++i) TEST_ASSERT_TRUE(service.emit(event));
  }
  debug::BatchSnapshot snapshot(uint64_t now = kStart) {
    debug::BatchSnapshot result{};
    TEST_ASSERT_TRUE(service.capture({boot(), worker.cursor()}, 8U, now, result));
    TEST_ASSERT_TRUE(result.count > 0U);
    TEST_ASSERT_TRUE(result.count <= result.events.size());
    return result;
  }
  void set_ack(const debug::BatchSnapshot& sent) {
    transport.ack = matching_ack(transport.body, sent);
    debug::UploadAck decoded{};
    TEST_ASSERT_TRUE(debug::decode_upload_ack(transport.ack, decoded));
    TEST_ASSERT_TRUE(debug::matches_upload_ack(
        decoded, "device-1", sent, telemetry_contract::sha256(transport.body)));
    transport.respond = true;
    transport.poll_result = network::TransportResult::Response;
  }
  void fail(uint64_t now, network::TransportResult result) {
    transport.respond = true;
    transport.poll_result = result;
    transport.quiet = true;
    worker.poll(now, ready);
  }
};

void test_cancel_retains_gate_and_frozen_payload_until_quiescent() {
  Fixture f;
  f.emit();
  f.worker.poll(kStart, f.ready);
  TEST_ASSERT_EQUAL_UINT32(1, f.transport.starts);
  TEST_ASSERT_EQUAL_UINT32(5000, f.transport.timeout_ms);
  TEST_ASSERT_TRUE(f.gate.active(debug::IoOwner::DebugHttp));
  const std::string body = f.transport.body;
  f.worker.cancel();
  f.worker.poll(kStart + 1U, f.ready);
  TEST_ASSERT_TRUE(f.transport.cancel_requested);
  TEST_ASSERT_TRUE(f.worker.transport_pending());
  TEST_ASSERT_FALSE(f.gate.try_acquire(debug::IoOwner::Filesystem));
  TEST_ASSERT_EQUAL_STRING(body.c_str(), std::string(f.transport.borrowed_body).c_str());
  TEST_ASSERT_EQUAL_STRING("token", std::string(f.transport.borrowed_token).c_str());
  f.transport.quiet = true;
  f.worker.poll(kStart + 2U, f.ready);
  TEST_ASSERT_FALSE(f.worker.transport_pending());
  TEST_ASSERT_TRUE(f.gate.try_acquire(debug::IoOwner::Filesystem));
  TEST_ASSERT_EQUAL_STRING(body.c_str(), f.transport.body.c_str());
}

void test_readiness_blocks_start_and_capture_is_not_empty_success() {
  Fixture f;
  f.worker.poll(kStart, {});
  TEST_ASSERT_EQUAL_UINT32(0, f.transport.starts);
  auto control = f.ready;
  control.control_active = true;
  f.worker.poll(kStart, control);
  TEST_ASSERT_EQUAL_UINT32(0, f.transport.starts);
  f.worker.poll(kStart, f.ready);
  TEST_ASSERT_EQUAL_UINT32(0, f.transport.starts);
  TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::Idle, f.worker.status());
}

void test_valid_synthetic_ack_waits_for_quiescence_and_delivers_once() {
  Fixture f;
  f.emit();
  const auto sent = f.snapshot();
  f.worker.poll(kStart, f.ready);
  f.set_ack(sent);
  f.worker.poll(kStart + 1U, f.ready);
  TEST_ASSERT_TRUE(f.worker.transport_pending());
  TEST_ASSERT_EQUAL_UINT64(0, f.worker.cursor());
  TEST_ASSERT_FALSE(f.gate.try_acquire(debug::IoOwner::Filesystem));
  const auto body_view = f.transport.borrowed_body;
  const auto token_view = f.transport.borrowed_token;
  const std::string body(body_view), token(token_view);
  f.worker.poll(kStart + 6U * kSecond, f.ready);
  TEST_ASSERT_TRUE(f.worker.transport_pending());
  TEST_ASSERT_EQUAL_UINT64(0, f.worker.cursor());
  TEST_ASSERT_FALSE(f.transport.cancel_requested);
  TEST_ASSERT_EQUAL_STRING(body.c_str(), std::string(body_view).c_str());
  TEST_ASSERT_EQUAL_STRING(token.c_str(), std::string(token_view).c_str());
  TEST_ASSERT_EQUAL_UINT32(1, f.transport.starts);
  f.transport.quiet = true;
  f.worker.poll(kStart + 6U * kSecond + 1U, f.ready);
  TEST_ASSERT_FALSE(f.worker.transport_pending());
  TEST_ASSERT_EQUAL_UINT64(sent.events[sent.count - 1U].sequence, f.worker.cursor());
  TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::Delivered, f.worker.status());
  f.worker.poll(kStart + 6U * kSecond + 2U, f.ready);
  TEST_ASSERT_EQUAL_UINT32(1, f.transport.starts);
}

void test_valid_matching_ack_first_observed_after_deadline_is_rejected() {
  Fixture f;
  f.emit();
  const auto sent = f.snapshot();
  f.worker.poll(kStart, f.ready);
  f.set_ack(sent);
  TEST_ASSERT_TRUE(f.transport.ack.size() <= 512U);
  const auto body_view = f.transport.borrowed_body;
  const auto token_view = f.transport.borrowed_token;
  const std::string body(body_view), token(token_view);
  f.worker.poll(kStart + 5U * kSecond + 1U, f.ready);
  TEST_ASSERT_TRUE(f.transport.cancel_requested);
  TEST_ASSERT_TRUE(f.worker.transport_pending());
  TEST_ASSERT_NOT_EQUAL(debug::DebugWorkerStatus::Delivered, f.worker.status());
  TEST_ASSERT_EQUAL_UINT64(0, f.worker.cursor());
  TEST_ASSERT_FALSE(f.gate.try_acquire(debug::IoOwner::Filesystem));
  TEST_ASSERT_EQUAL_UINT32(1, f.transport.starts);
  TEST_ASSERT_EQUAL_STRING(body.c_str(), std::string(body_view).c_str());
  TEST_ASSERT_EQUAL_STRING(token.c_str(), std::string(token_view).c_str());
  f.transport.quiet = true;
  f.worker.poll(kStart + 5U * kSecond + 2U, f.ready);
  TEST_ASSERT_FALSE(f.worker.transport_pending());
  TEST_ASSERT_EQUAL_UINT64(0, f.worker.cursor());
  TEST_ASSERT_NOT_EQUAL(debug::DebugWorkerStatus::Delivered, f.worker.status());
  TEST_ASSERT_EQUAL_UINT32(1, f.transport.starts);
  TEST_ASSERT_EQUAL_STRING(body.c_str(), std::string(body_view).c_str());
  TEST_ASSERT_EQUAL_STRING(token.c_str(), std::string(token_view).c_str());
}

void test_retries_keep_exact_snapshot_through_ring_overwrite() {
  Fixture f;
  f.emit(2U);
  const auto sent = f.snapshot();
  f.worker.poll(kStart, f.ready);
  const std::string frozen = f.transport.body;
  const auto digest = telemetry_contract::sha256(frozen);
  const auto borrowed = f.transport.borrowed_body;
  f.emit(80U, kStart);
  f.service.note_usb_loss(7U);
  f.service.note_delivery_loss(5U);
  TEST_ASSERT_TRUE(f.service.set_level(debug::Level::Warn));
  TEST_ASSERT_TRUE(f.service.meta(kStart).overwritten > 0U);
  TEST_ASSERT_EQUAL_STRING(frozen.c_str(), std::string(borrowed).c_str());
  f.fail(kStart + 1U, network::TransportResult::Failed);
  TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::RetryScheduled, f.worker.status());
  const uint64_t second = kStart + 1U + 5U * kSecond;
  f.worker.poll(second - 1U, f.ready);
  TEST_ASSERT_EQUAL_UINT32(1, f.transport.starts);
  f.worker.poll(second, f.ready);
  TEST_ASSERT_EQUAL_UINT32(2, f.transport.starts);
  TEST_ASSERT_EQUAL_STRING(frozen.c_str(), f.transport.body.c_str());
  TEST_ASSERT_TRUE(digest == telemetry_contract::sha256(f.transport.body));
  TEST_ASSERT_TRUE(borrowed.data() == f.transport.borrowed_body.data());
  f.fail(second + 1U, network::TransportResult::Timeout);
  const uint64_t third = second + 1U + 10U * kSecond;
  f.worker.poll(third - 1U, f.ready);
  TEST_ASSERT_EQUAL_UINT32(2, f.transport.starts);
  f.worker.poll(third, f.ready);
  TEST_ASSERT_EQUAL_UINT32(3, f.transport.starts);
  for (const auto& body : f.transport.bodies)
    TEST_ASSERT_EQUAL_STRING(frozen.c_str(), body.c_str());
  f.set_ack(sent);
  f.transport.quiet = true;
  f.worker.poll(third + 1U, f.ready);
  TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::Delivered, f.worker.status());
  TEST_ASSERT_EQUAL_UINT64(sent.events[sent.count - 1U].sequence, f.worker.cursor());
  TEST_ASSERT_EQUAL_UINT64(0, f.worker.delivery_loss());
  TEST_ASSERT_EQUAL_UINT64(5, f.service.meta(third).delivery_lost);
}

void test_three_attempt_budget_accounts_lost_events_once_without_consuming_sinks() {
  Fixture f;
  f.emit(2U);
  const auto sent = f.snapshot();
  const auto before = f.service.meta(kStart);
  uint64_t now = kStart;
  f.worker.poll(now, f.ready);
  f.fail(++now, network::TransportResult::Failed);
  now += 5U * kSecond;
  f.worker.poll(now, f.ready);
  f.fail(++now, network::TransportResult::Timeout);
  now += 10U * kSecond;
  f.worker.poll(now, f.ready);
  f.transport.respond = true;
  f.transport.poll_result = network::TransportResult::Failed;
  f.worker.poll(++now, f.ready);
  TEST_ASSERT_TRUE(f.worker.transport_pending());
  TEST_ASSERT_EQUAL_UINT64(0, f.worker.cursor());
  TEST_ASSERT_EQUAL_UINT64(0, f.worker.delivery_loss());
  TEST_ASSERT_EQUAL_UINT64(0, f.service.meta(now).delivery_lost);
  TEST_ASSERT_FALSE(f.gate.try_acquire(debug::IoOwner::Filesystem));
  now += 6U * kSecond;
  f.worker.poll(now, f.ready);
  TEST_ASSERT_TRUE(f.worker.transport_pending());
  TEST_ASSERT_EQUAL_UINT64(0, f.worker.delivery_loss());
  f.transport.quiet = true;
  f.worker.poll(++now, f.ready);
  TEST_ASSERT_EQUAL_UINT32(3, f.transport.starts);
  TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::DeliveryFailure, f.worker.status());
  TEST_ASSERT_EQUAL_UINT64(2, f.worker.cursor());
  TEST_ASSERT_EQUAL_UINT64(sent.count, f.worker.delivery_loss());
  TEST_ASSERT_EQUAL_UINT64(sent.count, f.service.meta(now).delivery_lost);
  for (unsigned i = 0; i < 3U; ++i) f.worker.poll(now + i, f.ready);
  f.worker.poll(now + 5U * kSecond - 1U, f.ready);
  TEST_ASSERT_EQUAL_UINT32(3, f.transport.starts);
  TEST_ASSERT_EQUAL_UINT64(sent.count, f.worker.delivery_loss());
  const auto after = f.service.meta(now);
  TEST_ASSERT_EQUAL_UINT64(before.latest, after.latest);
  TEST_ASSERT_EQUAL_UINT64(before.oldest, after.oldest);
  TEST_ASSERT_EQUAL_UINT64(before.usb_lost, after.usb_lost);
  const auto usb = f.service.read({boot(), 0U});
  const auto ble = f.service.read({boot(), 0U});
  TEST_ASSERT_TRUE(usb.has_event && ble.has_event);
  TEST_ASSERT_EQUAL_UINT64(1, usb.event.sequence);
  TEST_ASSERT_EQUAL_UINT64(1, ble.event.sequence);
  f.worker.poll(now + 5U * kSecond, f.ready);
  TEST_ASSERT_EQUAL_UINT32(3, f.transport.starts);
  f.emit();
  f.worker.poll(now + 5U * kSecond, f.ready);
  TEST_ASSERT_EQUAL_UINT32(4, f.transport.starts);
  TEST_ASSERT_FALSE(f.transport.bodies[0] == f.transport.bodies[3]);
  TEST_ASSERT_EQUAL_UINT64(sent.count, f.worker.delivery_loss());
}

void test_each_readiness_gate_blocks_and_recovers() {
  for (unsigned block = 0; block < 6U; ++block) {
    Fixture f;
    f.emit();
    auto unavailable = f.ready;
    switch (block) {
      case 0: unavailable.wifi = false; break;
      case 1: unavailable.time_valid = false; break;
      case 2: unavailable.ca_ready = false; break;
      case 3: unavailable.config_ready = false; break;
      case 4: unavailable.control_active = true; break;
      default: unavailable.other_transport_active = true; break;
    }
    f.worker.poll(kStart, unavailable);
    TEST_ASSERT_EQUAL_UINT32(0, f.transport.starts);
    TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::Unavailable, f.worker.status());
    f.worker.poll(kStart, f.ready);
    TEST_ASSERT_EQUAL_UINT32(1, f.transport.starts);
  }
}

void test_each_io_owner_blocks_debug_until_released() {
  for (const auto owner : {debug::IoOwner::ArchiveHttp, debug::IoOwner::ControlHttp,
                           debug::IoOwner::Filesystem}) {
    Fixture f;
    f.emit();
    TEST_ASSERT_TRUE(f.gate.try_acquire(owner));
    f.worker.poll(kStart, f.ready);
    TEST_ASSERT_EQUAL_UINT32(0, f.transport.starts);
    f.gate.release(owner);
    f.worker.poll(kStart, f.ready);
    TEST_ASSERT_EQUAL_UINT32(1, f.transport.starts);
    TEST_ASSERT_FALSE(f.gate.try_acquire(owner));
  }
}

void test_configuration_is_bounded_owned_and_immutable_while_borrowed() {
  Fixture f;
  f.emit();
  std::string endpoint = "https://example.invalid/debug";
  std::string token = "synthetic-token";
  TEST_ASSERT_TRUE(f.worker.configure(endpoint, token));
  endpoint.assign(endpoint.size(), 'x');
  token.assign(token.size(), 'x');
  TEST_ASSERT_EQUAL_STRING("https://example.invalid/debug", std::string(f.worker.endpoint()).c_str());
  f.worker.poll(kStart, f.ready);
  TEST_ASSERT_EQUAL_STRING("synthetic-token", std::string(f.transport.borrowed_token).c_str());
  TEST_ASSERT_FALSE(f.worker.configure("https://changed.invalid", "changed"));
  TEST_ASSERT_EQUAL_STRING("synthetic-token", std::string(f.transport.borrowed_token).c_str());
  TEST_ASSERT_EQUAL_UINT32(5000, f.transport.timeout_ms);
}

void test_invalid_endpoint_and_token_do_not_configure_or_start() {
  const std::vector<std::string> endpoints = {
      "", "http://example.invalid", "https://", std::string(193U, 'x'),
      "https://" + std::string(185U, 'x')};
  for (const auto& endpoint : endpoints) {
    Fixture f;
    debug::DebugHttpWorker unconfigured(f.service, f.transport, f.gate, "device-1");
    f.emit();
    TEST_ASSERT_FALSE(unconfigured.configure(endpoint, "token"));
    unconfigured.poll(kStart, f.ready);
    TEST_ASSERT_EQUAL_UINT32(0, f.transport.starts);
  }
  for (const auto& token : {std::string{}, std::string(129U, 'x')}) {
    Fixture f;
    debug::DebugHttpWorker unconfigured(f.service, f.transport, f.gate, "device-1");
    f.emit();
    TEST_ASSERT_FALSE(unconfigured.configure("https://example.invalid", token));
    unconfigured.poll(kStart, f.ready);
    TEST_ASSERT_EQUAL_UINT32(0, f.transport.starts);
  }
}

void test_sticky_http_rejections_survive_reconnect_and_same_configuration() {
  for (const int status : {202, 400, 401, 409, 422}) {
    Fixture f;
    f.emit();
    const auto sent = f.snapshot();
    f.worker.poll(kStart, f.ready);
    f.set_ack(sent);
    f.transport.http_status = status;
    f.transport.quiet = true;
    f.worker.poll(kStart + 1U, f.ready);
    TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::Stopped, f.worker.status());
    f.ready.wifi = false;
    f.worker.poll(kStart + 20U * kSecond, f.ready);
    f.ready.wifi = true;
    TEST_ASSERT_TRUE(f.worker.configure("https://example.invalid/", "token"));
    f.worker.poll(kStart + 30U * kSecond, f.ready);
    TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::Stopped, f.worker.status());
    TEST_ASSERT_EQUAL_UINT32(1, f.transport.starts);
    TEST_ASSERT_EQUAL_UINT64(0, f.worker.cursor());
    TEST_ASSERT_TRUE(f.worker.configure("https://example.invalid/", "new-token"));
    f.worker.poll(kStart + 30U * kSecond, f.ready);
    TEST_ASSERT_EQUAL_UINT32(2, f.transport.starts);
  }
}

void test_transient_http_statuses_retry_the_same_body_at_five_seconds() {
  for (const int status : {408, 429, 500, 503, 599}) {
    Fixture f;
    f.emit();
    const auto sent = f.snapshot();
    f.worker.poll(kStart, f.ready);
    const std::string body = f.transport.body;
    f.set_ack(sent);
    f.transport.http_status = status;
    f.transport.quiet = true;
    f.worker.poll(kStart + 1U, f.ready);
    TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::RetryScheduled, f.worker.status());
    f.worker.poll(kStart + 5U * kSecond, f.ready);
    TEST_ASSERT_EQUAL_UINT32(1, f.transport.starts);
    f.worker.poll(kStart + 1U + 5U * kSecond, f.ready);
    TEST_ASSERT_EQUAL_UINT32(2, f.transport.starts);
    TEST_ASSERT_EQUAL_STRING(body.c_str(), f.transport.body.c_str());
  }
}

void test_start_response_is_observed_before_deadline_but_cleanup_can_finish_late() {
  Fixture f;
  f.emit();
  const auto sent = f.snapshot();
  char encoded[4096];
  size_t size{};
  TEST_ASSERT_TRUE(debug::encode_upload("device-1", sent, encoded, sizeof(encoded), size));
  f.transport.ack = matching_ack({encoded, size}, sent);
  f.transport.start_result = network::TransportResult::Response;
  f.transport.respond = true;
  f.worker.poll(kStart, f.ready);
  TEST_ASSERT_EQUAL_UINT32(1, f.transport.polls);
  TEST_ASSERT_TRUE(f.worker.transport_pending());
  f.worker.poll(kStart + 6U * kSecond, f.ready);
  TEST_ASSERT_FALSE(f.transport.cancel_requested);
  TEST_ASSERT_EQUAL_UINT64(0, f.worker.cursor());
  TEST_ASSERT_FALSE(f.gate.try_acquire(debug::IoOwner::Filesystem));
  f.transport.quiet = true;
  f.worker.poll(kStart + 6U * kSecond + 1U, f.ready);
  TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::Delivered, f.worker.status());
}
void test_successful_batches_obey_exact_five_second_cadence() {
  Fixture f;
  f.emit();
  const auto sent = f.snapshot();
  f.worker.poll(kStart, f.ready);
  f.set_ack(sent);
  f.transport.quiet = true;
  f.worker.poll(kStart + 1U, f.ready);
  TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::Delivered, f.worker.status());
  f.emit();
  f.worker.poll(kStart + 5U * kSecond, f.ready);
  TEST_ASSERT_EQUAL_UINT32(1, f.transport.starts);
  f.worker.poll(kStart + 1U + 5U * kSecond, f.ready);
  TEST_ASSERT_EQUAL_UINT32(2, f.transport.starts);
}

void test_readiness_loss_cancels_once_and_rejects_an_available_ack() {
  for (unsigned block = 0; block < 6U; ++block) {
    Fixture f;
    f.emit();
    const auto sent = f.snapshot();
    f.worker.poll(kStart, f.ready);
    f.set_ack(sent);
    auto unavailable = f.ready;
    switch (block) {
      case 0: unavailable.wifi = false; break;
      case 1: unavailable.time_valid = false; break;
      case 2: unavailable.ca_ready = false; break;
      case 3: unavailable.config_ready = false; break;
      case 4: unavailable.control_active = true; break;
      default: unavailable.other_transport_active = true; break;
    }
    const std::string body = f.transport.body;
    f.worker.poll(kStart + 1U, unavailable);
    f.worker.poll(kStart + 2U, f.ready);
    TEST_ASSERT_EQUAL_UINT32(1, f.transport.cancels);
    TEST_ASSERT_EQUAL_UINT32(0, f.transport.polls);
    TEST_ASSERT_TRUE(f.worker.transport_pending());
    TEST_ASSERT_FALSE(f.gate.try_acquire(debug::IoOwner::ArchiveHttp));
    TEST_ASSERT_FALSE(f.gate.try_acquire(debug::IoOwner::ControlHttp));
    TEST_ASSERT_FALSE(f.gate.try_acquire(debug::IoOwner::Filesystem));
    TEST_ASSERT_EQUAL_STRING(body.c_str(), std::string(f.transport.borrowed_body).c_str());
    TEST_ASSERT_EQUAL_STRING("token", std::string(f.transport.borrowed_token).c_str());
    f.transport.quiet = true;
    f.worker.poll(kStart + 3U, f.ready);
    TEST_ASSERT_FALSE(f.worker.transport_pending());
    TEST_ASSERT_EQUAL_UINT64(0, f.worker.cursor());
    TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::RetryScheduled, f.worker.status());
    TEST_ASSERT_TRUE(f.gate.try_acquire(debug::IoOwner::Filesystem));
  }
}

void test_deadline_boundary_cancels_without_polling_a_late_result() {
  Fixture f;
  f.emit();
  const auto sent = f.snapshot();
  f.worker.poll(kStart, f.ready);
  f.worker.poll(kStart + 5U * kSecond - 1U, f.ready);
  TEST_ASSERT_FALSE(f.transport.cancel_requested);
  TEST_ASSERT_EQUAL_UINT32(1, f.transport.polls);
  f.set_ack(sent);
  f.worker.poll(kStart + 5U * kSecond, f.ready);
  TEST_ASSERT_TRUE(f.transport.cancel_requested);
  TEST_ASSERT_EQUAL_UINT32(1, f.transport.polls);
  TEST_ASSERT_EQUAL_UINT64(0, f.worker.cursor());
  TEST_ASSERT_TRUE(f.worker.transport_pending());
  f.transport.quiet = true;
  f.worker.poll(kStart + 5U * kSecond + 1U, f.ready);
  TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::RetryScheduled, f.worker.status());
}

void test_immediate_transport_failures_retain_gate_until_quiescent() {
  for (const auto result : {network::TransportResult::Failed,
                            network::TransportResult::Timeout}) {
    Fixture f;
    f.emit();
    f.transport.start_result = result;
    f.worker.poll(kStart, f.ready);
    TEST_ASSERT_TRUE(f.worker.transport_pending());
    TEST_ASSERT_FALSE(f.gate.try_acquire(debug::IoOwner::Filesystem));
    f.worker.poll(kStart + 6U * kSecond, f.ready);
    TEST_ASSERT_EQUAL_UINT32(0, f.transport.cancels);
    TEST_ASSERT_TRUE(f.worker.transport_pending());
    f.transport.quiet = true;
    f.worker.poll(kStart + 6U * kSecond + 1U, f.ready);
    TEST_ASSERT_FALSE(f.worker.transport_pending());
    TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::RetryScheduled, f.worker.status());
    TEST_ASSERT_EQUAL_UINT64(0, f.worker.cursor());
    TEST_ASSERT_TRUE(f.gate.try_acquire(debug::IoOwner::Filesystem));
  }
}

void replace_once(std::string& packet, std::string_view from, std::string_view to) {
  const auto at = packet.find(from);
  TEST_ASSERT_TRUE(at != std::string::npos);
  packet.replace(at, from.size(), to.data(), to.size());
}

void test_each_valid_but_wrong_ack_binding_stops_without_advancing() {
  for (unsigned field = 0; field < 6U; ++field) {
    Fixture f;
    f.emit(2U);
    const auto sent = f.snapshot();
    f.worker.poll(kStart, f.ready);
    f.set_ack(sent);
    switch (field) {
      case 0:
        replace_once(f.transport.ack, "\"device_id\":\"device-1\"", "\"device_id\":\"device-2\"");
        break;
      case 1:
        replace_once(f.transport.ack, "\"boot_id\":\"00000000000000000000000000000001\"",
                     "\"boot_id\":\"00000000000000000000000000000002\"");
        break;
      case 2:
        replace_once(f.transport.ack, "\"first_sequence\":\"1\"", "\"first_sequence\":\"2\"");
        break;
      case 3:
        replace_once(f.transport.ack, "\"last_sequence\":\"2\"", "\"last_sequence\":\"3\"");
        break;
      case 4:
        replace_once(f.transport.ack, "\"stored_count\":2", "\"stored_count\":1");
        break;
      default: {
        const auto at = f.transport.ack.find("\"request_sha256\":\"");
        TEST_ASSERT_TRUE(at != std::string::npos);
        auto& digit = f.transport.ack[at + std::string("\"request_sha256\":\"").size()];
        digit = digit == '0' ? '1' : '0';
        break;
      }
    }
    debug::UploadAck decoded{};
    TEST_ASSERT_TRUE(debug::decode_upload_ack(f.transport.ack, decoded));
    TEST_ASSERT_FALSE(debug::matches_upload_ack(
        decoded, "device-1", sent, telemetry_contract::sha256(f.transport.body)));
    f.worker.poll(kStart + 1U, f.ready);
    TEST_ASSERT_TRUE(f.worker.transport_pending());
    TEST_ASSERT_EQUAL_UINT64(0, f.worker.cursor());
    TEST_ASSERT_FALSE(f.gate.try_acquire(debug::IoOwner::Filesystem));
    f.set_ack(sent);  // The worker must retain its already-observed result copy.
    f.transport.quiet = true;
    f.worker.poll(kStart + 6U * kSecond, f.ready);
    TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::Stopped, f.worker.status());
    TEST_ASSERT_EQUAL_UINT64(0, f.worker.cursor());
    f.ready.wifi = false;
    f.worker.poll(kStart + 10U * kSecond, f.ready);
    f.ready.wifi = true;
    f.worker.poll(kStart + 20U * kSecond, f.ready);
    TEST_ASSERT_EQUAL_UINT32(1, f.transport.starts);
  }
}

void test_truncated_and_oversized_ack_are_rejected_without_overflowing_fake() {
  for (unsigned kind = 0; kind < 2U; ++kind) {
    Fixture f;
    f.emit();
    const auto sent = f.snapshot();
    f.worker.poll(kStart, f.ready);
    f.set_ack(sent);
    TEST_ASSERT_TRUE(f.transport.ack.size() <= 512U);
    if (kind == 0U) f.transport.truncated = true;
    else f.transport.reported_size = 513U;
    f.transport.quiet = true;
    f.worker.poll(kStart + 1U, f.ready);
    TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::Stopped, f.worker.status());
    TEST_ASSERT_EQUAL_UINT64(0, f.worker.cursor());
    TEST_ASSERT_EQUAL_UINT64(0, f.worker.delivery_loss());
    TEST_ASSERT_FALSE(f.worker.transport_pending());
  }
}

void test_malformed_ack_is_a_separate_protocol_stop() {
  Fixture f;
  f.emit();
  const auto sent = f.snapshot();
  f.worker.poll(kStart, f.ready);
  f.set_ack(sent);
  f.transport.ack = "{";
  f.transport.quiet = true;
  f.worker.poll(kStart + 1U, f.ready);
  TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::Stopped, f.worker.status());
  TEST_ASSERT_EQUAL_UINT64(0, f.worker.cursor());
}

void test_deduplicated_success_uses_sum_of_ack_counts() {
  Fixture f;
  f.emit(2U);
  const auto sent = f.snapshot();
  f.worker.poll(kStart, f.ready);
  f.set_ack(sent);
  replace_once(f.transport.ack, "\"stored_count\":2", "\"stored_count\":0");
  replace_once(f.transport.ack, "\"deduplicated_count\":0", "\"deduplicated_count\":2");
  debug::UploadAck decoded{};
  TEST_ASSERT_TRUE(debug::decode_upload_ack(f.transport.ack, decoded));
  TEST_ASSERT_TRUE(debug::matches_upload_ack(
      decoded, "device-1", sent, telemetry_contract::sha256(f.transport.body)));
  f.transport.quiet = true;
  f.worker.poll(kStart + 1U, f.ready);
  TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::Delivered, f.worker.status());
  TEST_ASSERT_EQUAL_UINT64(2, f.worker.cursor());
}

void test_unconfigured_or_invalid_identity_cannot_start() {
  for (const auto& identity : {std::string{}, std::string(65U, 'x'),
                               std::string("\xc0\x80", 2U)}) {
    Fixture f;
    f.emit();
    debug::DebugHttpWorker worker(f.service, f.transport, f.gate, identity);
    worker.poll(kStart, f.ready);
    TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::Unconfigured, worker.status());
    worker.configure("https://example.invalid", "token");
    worker.poll(kStart, f.ready);
    TEST_ASSERT_EQUAL_UINT32(0, f.transport.starts);
    TEST_ASSERT_EQUAL_UINT64(0, worker.cursor());
  }
}

void test_maximum_config_bounds_and_eight_event_request_match_actual_encoder() {
  Fixture f;
  f.emit(9U);
  std::string identity(64U, 'd');
  std::string endpoint = "https://example.invalid/";
  endpoint.resize(192U, 'p');
  const std::string token(128U, 't');
  debug::DebugHttpWorker worker(f.service, f.transport, f.gate, identity);
  TEST_ASSERT_TRUE(worker.configure(endpoint, token));
  identity.assign(64U, 'x');
  const auto sent = f.snapshot();
  TEST_ASSERT_EQUAL_UINT32(8, sent.count);
  char body[4096];
  size_t size{};
  TEST_ASSERT_TRUE(debug::encode_upload(std::string(64U, 'd'), sent, body, sizeof(body), size));
  worker.poll(kStart, f.ready);
  TEST_ASSERT_EQUAL_UINT32(1, f.transport.starts);
  TEST_ASSERT_EQUAL_UINT32(128, f.transport.borrowed_token.size());
  TEST_ASSERT_EQUAL_UINT32(192, worker.endpoint().size());
  TEST_ASSERT_TRUE(f.transport.body == std::string(body, size));
  TEST_ASSERT_TRUE(f.transport.body.size() <= 4096U);
}
void test_wide_service_snapshot_is_encoded_without_truncation_or_number_narrowing() {
  Lock lock;
  const std::string identity(64U, '\x01');
  const std::string build(96U, '\\');
  debug::DebugService service(boot(), build, debug::Level::Debug, lock,
      acquisition::SequenceAllocator{UINT64_MAX - 72U});
  constexpr uint64_t now = UINT64_MAX - 10U * kSecond;
  debug::EventInput event{};
  event.monotonic_us = now;
  event.level = debug::Level::Debug;
  event.module = debug::Module::Runtime;
  event.event_code = 1;
  event.presence = debug::kHasRequestId | debug::kHasRelatedSequence;
  event.request_id = UINT32_MAX;
  event.duration_us = UINT32_MAX;
  event.related_sequence = UINT64_MAX;
  event.arg0 = INT32_MIN;
  event.arg1 = INT32_MIN;
  for (unsigned i = 0; i < 72U; ++i) TEST_ASSERT_TRUE(service.emit(event));
  service.note_delivery_loss(UINT64_MAX);
  debug::BatchSnapshot sent{};
  TEST_ASSERT_TRUE(service.capture({boot(), 0U}, 8U, now, sent));
  TEST_ASSERT_EQUAL_UINT32(8, sent.count);
  TEST_ASSERT_TRUE(sent.meta.overwritten > 0U);
  char encoded[4096];
  size_t size{};
  TEST_ASSERT_TRUE(debug::encode_upload(identity, sent, encoded, sizeof(encoded), size));
  TEST_ASSERT_TRUE(size <= sizeof(encoded));
  Transport transport;
  debug::RuntimeIoGate gate;
  debug::DebugHttpWorker worker(service, transport, gate, identity);
  TEST_ASSERT_TRUE(worker.configure("https://example.invalid", "token"));
  worker.poll(now, {true, true, true, true, false, false});
  TEST_ASSERT_EQUAL_UINT32(1, transport.starts);
  TEST_ASSERT_TRUE(transport.body == std::string(encoded, size));
  TEST_ASSERT_TRUE(transport.body.find("\\u0001") != std::string::npos);
  TEST_ASSERT_TRUE(transport.body.find(std::to_string(UINT64_MAX)) != std::string::npos);
}
void test_same_configuration_preserves_retry_schedule_status() {
  Fixture f;
  f.emit();
  f.worker.poll(kStart, f.ready);
  f.fail(kStart + 1U, network::TransportResult::Failed);
  TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::RetryScheduled, f.worker.status());
  TEST_ASSERT_TRUE(f.worker.configure("https://example.invalid/", "token"));
  TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::RetryScheduled, f.worker.status());
  f.worker.poll(kStart + 2U, f.ready);
  TEST_ASSERT_EQUAL_UINT32(1, f.transport.starts);
}

void test_borrowing_worker_cannot_be_copied_or_moved_to_another_buffer_owner() {
  TEST_ASSERT_FALSE(std::is_copy_constructible<debug::DebugHttpWorker>::value);
  TEST_ASSERT_FALSE(std::is_move_constructible<debug::DebugHttpWorker>::value);
}
void test_malformed_authority_or_url_whitespace_never_starts() {
  std::vector<std::string> invalid = {
      "https://?x", "https://#x", "https:///path", "https://",
      "https://exa mple.invalid", "https://example.invalid/path with space"};
  for (unsigned byte = 0U; byte <= 0x20U; ++byte) {
    std::string endpoint = "https://example.invalid/path";
    endpoint.push_back(static_cast<char>(byte));
    endpoint += "suffix";
    invalid.push_back(endpoint);
  }
  invalid.push_back(std::string("https://example.invalid/") + static_cast<char>(0x7f));
  for (const auto& endpoint : invalid) {
    Fixture f;
    f.emit();
    debug::DebugHttpWorker worker(f.service, f.transport, f.gate, "device-1");
    const bool configured = worker.configure(endpoint, "token");
    worker.poll(kStart, f.ready);
    TEST_ASSERT_EQUAL_UINT32(0, f.transport.starts);
    TEST_ASSERT_FALSE(configured);
    TEST_ASSERT_EQUAL(debug::DebugWorkerStatus::Unconfigured, worker.status());
  }
}

void test_valid_authorities_and_encoded_whitespace_remain_supported() {
  for (const auto* endpoint : {
      "https://example.invalid", "https://example.invalid:8443/path?x=1#part",
      "https://127.0.0.1/debug", "https://[::1]:8443/debug",
      "https://example.invalid/path%20part?x=%09"}) {
    Fixture f;
    f.emit();
    TEST_ASSERT_TRUE(f.worker.configure(endpoint, "token"));
    TEST_ASSERT_TRUE(f.worker.endpoint() == endpoint);
    f.worker.poll(kStart, f.ready);
    TEST_ASSERT_EQUAL_UINT32(1, f.transport.starts);
  }
}
}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_cancel_retains_gate_and_frozen_payload_until_quiescent);
  RUN_TEST(test_readiness_blocks_start_and_capture_is_not_empty_success);
  RUN_TEST(test_valid_synthetic_ack_waits_for_quiescence_and_delivers_once);
  RUN_TEST(test_valid_matching_ack_first_observed_after_deadline_is_rejected);
  RUN_TEST(test_retries_keep_exact_snapshot_through_ring_overwrite);
  RUN_TEST(test_three_attempt_budget_accounts_lost_events_once_without_consuming_sinks);
  RUN_TEST(test_each_readiness_gate_blocks_and_recovers);
  RUN_TEST(test_each_io_owner_blocks_debug_until_released);
  RUN_TEST(test_configuration_is_bounded_owned_and_immutable_while_borrowed);
  RUN_TEST(test_invalid_endpoint_and_token_do_not_configure_or_start);
  RUN_TEST(test_sticky_http_rejections_survive_reconnect_and_same_configuration);
  RUN_TEST(test_transient_http_statuses_retry_the_same_body_at_five_seconds);
  RUN_TEST(test_start_response_is_observed_before_deadline_but_cleanup_can_finish_late);
  RUN_TEST(test_successful_batches_obey_exact_five_second_cadence);
  RUN_TEST(test_readiness_loss_cancels_once_and_rejects_an_available_ack);
  RUN_TEST(test_deadline_boundary_cancels_without_polling_a_late_result);
  RUN_TEST(test_immediate_transport_failures_retain_gate_until_quiescent);
  RUN_TEST(test_each_valid_but_wrong_ack_binding_stops_without_advancing);
  RUN_TEST(test_truncated_and_oversized_ack_are_rejected_without_overflowing_fake);
  RUN_TEST(test_malformed_ack_is_a_separate_protocol_stop);
  RUN_TEST(test_deduplicated_success_uses_sum_of_ack_counts);
  RUN_TEST(test_unconfigured_or_invalid_identity_cannot_start);
  RUN_TEST(test_maximum_config_bounds_and_eight_event_request_match_actual_encoder);
  RUN_TEST(test_wide_service_snapshot_is_encoded_without_truncation_or_number_narrowing);
  RUN_TEST(test_same_configuration_preserves_retry_schedule_status);
  RUN_TEST(test_borrowing_worker_cannot_be_copied_or_moved_to_another_buffer_owner);
  RUN_TEST(test_malformed_authority_or_url_whitespace_never_starts);
  RUN_TEST(test_valid_authorities_and_encoded_whitespace_remain_supported);
  return UNITY_END();
}
