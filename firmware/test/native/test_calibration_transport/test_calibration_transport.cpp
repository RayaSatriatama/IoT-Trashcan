#include <unity.h>

#include "calibration/calibration_transport.h"

namespace {

using namespace sigap::calibration;

class FakeWorkflow final : public ICalibrationTransportWorkflow {
 public:
  bool status(CalibrationStatusSnapshot& output) const noexcept override {
    output = status_;
    return true;
  }
  bool evidence(CalibrationTarget target,
                DiagnosticEvidence& output) const noexcept override {
    output = evidence_;
    output.sensor = static_cast<sigap::acquisition::SensorId>(target);
    return evidence_available_;
  }
  bool active_profile(CalibrationTarget,
                      CalibrationProfileRef& output) const noexcept override {
    output = profile;
    return true;
  }
  bool start(CalibrationTarget target, uint64_t now_us) noexcept override {
    if (status_.active) return false;
    status_.active = true;
    status_.state = CalibrationState::Capture;
    status_.target = target;
    status_.last_activity_us = now_us;
    ++start_calls;
    return true;
  }
  bool set_criteria(const CaptureCriteria&, uint64_t now_us) noexcept override {
    status_.last_activity_us = now_us;
    return status_.active;
  }
  bool capture(CalibrationAnchor, uint64_t now_us) noexcept override {
    status_.last_activity_us = now_us;
    if (!status_.active) return false;
    ++status_.accepted_samples;
    return true;
  }
  bool validate() noexcept override {
    status_.state = CalibrationState::Preview;
    return status_.active;
  }
  bool preview(ValidationResult& output) const noexcept override {
    output.code = ValidationCode::Valid;
    return status_.active;
  }
  bool commit() noexcept override {
    if (!status_.active) return false;
    status_.active = false;
    status_.state = CalibrationState::Idle;
    profile.present = true;
    ++profile.version;
    return true;
  }
  bool cancel() noexcept override {
    if (!status_.active) return false;
    status_.active = false;
    status_.state = CalibrationState::Idle;
    ++cancel_calls;
    return true;
  }
  bool history(CalibrationResultEnvelope&) const noexcept override { return true; }
  bool rollback() noexcept override { return true; }

  mutable CalibrationStatusSnapshot status_{};
  CalibrationProfileRef profile{};
  DiagnosticEvidence evidence_{};
  bool evidence_available_{true};
  uint32_t start_calls{0U};
  uint32_t cancel_calls{0U};
};

class AllowAuthorizer final : public ICalibrationTransportAuthorizer {
 public:
  bool authorize(const CalibrationCommandEnvelope&) const noexcept override {
    return allow;
  }
  bool allow{true};
};

CalibrationCommandEnvelope command(CalibrationOperation operation,
                                   CalibrationTransport transport,
                                   uint64_t request_id,
                                   uint64_t session_id = 0U) {
  CalibrationCommandEnvelope value{};
  value.operation = operation;
  value.transport = transport;
  value.request_id = request_id;
  value.device_id = 0x1234U;
  value.session_id = session_id;
  value.target = CalibrationTarget::Hx711Organic;
  return value;
}

void test_owner_and_idempotent_start() {
  FakeWorkflow workflow{};
  AllowAuthorizer authorizer{};
  CalibrationTransportController controller(workflow, 0x1234U, &authorizer);
  CalibrationResultEnvelope result{};
  auto start = command(CalibrationOperation::Start, CalibrationTransport::Ble, 1U);
  TEST_ASSERT_TRUE(controller.execute(start, 10U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Accepted),
                        static_cast<int>(result.status));
  const uint64_t session = result.session_id;
  TEST_ASSERT_TRUE(controller.execute(start, 11U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Replayed),
                        static_cast<int>(result.status));
  TEST_ASSERT_EQUAL_UINT32(1U, workflow.start_calls);

  auto competing = command(CalibrationOperation::Start,
                            CalibrationTransport::Https, 2U);
  TEST_ASSERT_FALSE(controller.execute(competing, 12U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::SessionBusy),
                        static_cast<int>(result.status));

  auto cancel = command(CalibrationOperation::Cancel, CalibrationTransport::Ble,
                        3U, session);
  TEST_ASSERT_TRUE(controller.execute(cancel, 13U, result));
}

void test_expiry_cancels_workflow_and_disconnect_is_local() {
  FakeWorkflow workflow{};
  CalibrationTransportController controller(workflow, 0x1234U);
  CalibrationResultEnvelope result{};
  auto start = command(CalibrationOperation::Start, CalibrationTransport::Usb, 10U);
  TEST_ASSERT_TRUE(controller.execute(start, 100U, result));
  controller.poll(100U + CalibrationSession::kSessionTimeoutUs + 1U);
  TEST_ASSERT_FALSE(controller.active());
  TEST_ASSERT_EQUAL_UINT32(1U, workflow.cancel_calls);

  start.request_id = 11U;
  TEST_ASSERT_TRUE(controller.execute(start, 200U, result));
  controller.disconnect(CalibrationTransport::Ble);
  TEST_ASSERT_TRUE(controller.active());
  controller.disconnect(CalibrationTransport::Usb);
  TEST_ASSERT_FALSE(controller.active());
}

void test_expected_profile_conflict_and_preview() {
  FakeWorkflow workflow{};
  workflow.profile.present = true;
  workflow.profile.version = 7U;
  AllowAuthorizer authorizer{};
  CalibrationTransportController controller(workflow, 0x1234U, &authorizer);
  CalibrationResultEnvelope result{};
  auto start = command(CalibrationOperation::Start, CalibrationTransport::Https, 20U);
  TEST_ASSERT_TRUE(controller.execute(start, 1U, result));
  auto preview = command(CalibrationOperation::Preview, CalibrationTransport::Https,
                         21U, result.session_id);
  preview.expected_profile_present = true;
  preview.expected_profile.present = true;
  preview.expected_profile.version = 6U;
  TEST_ASSERT_FALSE(controller.execute(preview, 2U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Conflict),
                        static_cast<int>(result.status));

  preview.request_id = 22U;
  preview.expected_profile = workflow.profile;
  TEST_ASSERT_TRUE(controller.execute(preview, 3U, result));
  TEST_ASSERT_TRUE(result.preview_available);
}

void test_remote_transport_requires_authorization() {
  FakeWorkflow workflow{};
  CalibrationTransportController controller(workflow, 0x1234U);
  CalibrationResultEnvelope result{};
  const auto status = command(CalibrationOperation::GetStatus,
                              CalibrationTransport::Https, 30U);
  TEST_ASSERT_FALSE(controller.execute(status, 1U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Unauthorized),
                        static_cast<int>(result.status));
}

void test_expiry_and_device_binding_are_rejected() {
  FakeWorkflow workflow{};
  CalibrationTransportController controller(workflow, 99U);
  CalibrationResultEnvelope result{};
  auto invalid = command(CalibrationOperation::GetStatus, CalibrationTransport::Usb, 31U);
  invalid.device_id = 100U;
  TEST_ASSERT_FALSE(controller.execute(invalid, 1U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Invalid),
                        static_cast<int>(result.status));
  invalid.device_id = 99U;
  invalid.expires_at_us = 1U;
  TEST_ASSERT_FALSE(controller.execute(invalid, 2U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Expired),
                        static_cast<int>(result.status));
}

void test_request_id_reuse_with_changed_payload_is_conflict() {
  FakeWorkflow workflow{};
  AllowAuthorizer authorizer{};
  CalibrationTransportController controller(workflow, 0x1234U, &authorizer);
  CalibrationResultEnvelope result{};
  auto start = command(CalibrationOperation::Start, CalibrationTransport::Ble, 40U);
  start.expires_at_us = 1000U;
  TEST_ASSERT_TRUE(controller.execute(start, 1U, result));
  start.expires_at_us = 2000U;
  TEST_ASSERT_FALSE(controller.execute(start, 2U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Conflict),
                        static_cast<int>(result.status));
  TEST_ASSERT_EQUAL_UINT32(1U, workflow.start_calls);
}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_owner_and_idempotent_start);
  RUN_TEST(test_expiry_cancels_workflow_and_disconnect_is_local);
  RUN_TEST(test_expected_profile_conflict_and_preview);
  RUN_TEST(test_remote_transport_requires_authorization);
  RUN_TEST(test_expiry_and_device_binding_are_rejected);
  RUN_TEST(test_request_id_reuse_with_changed_payload_is_conflict);
  UNITY_END();
}

void loop() {}

int main() {
  setup();
  return 0;
}
