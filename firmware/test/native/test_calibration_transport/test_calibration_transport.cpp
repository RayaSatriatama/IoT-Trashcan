#include <unity.h>

#include "calibration/calibration_replay_store.h"
#include "calibration/calibration_transport.h"
#include "debug/types.h"

namespace {

using namespace sigap::calibration;

class EventSink final : public sigap::debug::IEventSink {
 public:
  bool emit(const sigap::debug::EventInput& input) noexcept override {
    if (count < events.size()) events[count++] = input;
    return true;
  }
  std::array<sigap::debug::EventInput, 16U> events{};
  std::size_t count{};
};

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
  bool proposed_profile(CalibrationProfileRef& output) const noexcept override {
    output = proposal;
    return proposal.present;
  }
  bool start(CalibrationTarget target, uint64_t now_us) noexcept override {
    ++start_attempts;
    if (!start_allowed || status_.active) return false;
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
  bool preview(ValidationResult& output) noexcept override {
    output.code = ValidationCode::Valid;
    return status_.active;
  }
  bool commit() noexcept override {
    if (!status_.active) return false;
    status_.active = false;
    status_.state = CalibrationState::Idle;
    profile.present = true;
    ++profile.version;
    ++commit_calls;
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
  CalibrationProfileRef proposal{};
  DiagnosticEvidence evidence_{};
  bool evidence_available_{true};
  uint32_t start_calls{0U};
  uint32_t start_attempts{0U};
  bool start_allowed{true};
  uint32_t commit_calls{0U};
  uint32_t cancel_calls{0U};
};

class AllowAuthorizer final : public ICalibrationTransportAuthorizer {
 public:
  bool authorize(const CalibrationCommandEnvelope&) const noexcept override {
    return allow;
  }
  bool allow{true};
};

class ToggleAuthorizer final : public ICalibrationTransportAuthorizer {
 public:
  bool authorize(const CalibrationCommandEnvelope&) const noexcept override {
    return allowed;
  }
  bool allowed{true};
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

void test_session_owner_binds_authenticated_principal() {
  FakeWorkflow workflow{};
  AllowAuthorizer authorizer{};
  CalibrationTransportController controller(workflow, 0x1234U, &authorizer);
  CalibrationResultEnvelope result{};
  auto start = command(CalibrationOperation::Start, CalibrationTransport::Https, 40U);
  start.principal_id = 11U;
  TEST_ASSERT_TRUE(controller.execute(start, 10U, result));
  auto criteria = command(CalibrationOperation::SetCriteria,
                          CalibrationTransport::Https, 41U, result.session_id);
  criteria.principal_id = 12U;
  criteria.criteria_present = true;
  TEST_ASSERT_FALSE(controller.execute(criteria, 11U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::SessionBusy),
                        static_cast<int>(result.status));
}

void test_expiry_cancels_workflow_and_disconnect_is_local() {
  FakeWorkflow workflow{};
  EventSink sink{};
  CalibrationTransportController controller(workflow, 0x1234U);
  controller.set_debug_sink(&sink);
  CalibrationResultEnvelope result{};
  auto start = command(CalibrationOperation::Start, CalibrationTransport::Usb, 10U);
  TEST_ASSERT_TRUE(controller.execute(start, 100U, result));
  controller.poll(100U + CalibrationSession::kSessionTimeoutUs + 1U);
  TEST_ASSERT_FALSE(controller.active());
  TEST_ASSERT_EQUAL_UINT32(1U, workflow.cancel_calls);
  TEST_ASSERT_EQUAL_UINT32(4U, sink.count);
  TEST_ASSERT_EQUAL_UINT32(50U, sink.events[2].event_code);
  TEST_ASSERT_EQUAL_INT(4, sink.events[2].arg1);
  TEST_ASSERT_EQUAL_UINT32(51U, sink.events[3].event_code);
  TEST_ASSERT_EQUAL_INT(0, sink.events[3].arg0);

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

void test_local_deadline_is_not_replay_identity() {
  FakeWorkflow workflow{};
  AllowAuthorizer authorizer{};
  CalibrationTransportController controller(workflow, 0x1234U, &authorizer);
  CalibrationResultEnvelope result{};
  auto start = command(CalibrationOperation::Start, CalibrationTransport::Ble, 40U);
  start.expires_at_us = 1000U;
  TEST_ASSERT_TRUE(controller.execute(start, 1U, result));
  start.expires_at_us = 2000U;
  TEST_ASSERT_TRUE(controller.execute(start, 2U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Replayed),
                        static_cast<int>(result.status));
  TEST_ASSERT_EQUAL_UINT32(1U, workflow.start_calls);
}

void test_failed_replay_persistence_is_reconciled_without_second_commit() {
  MemoryCalibrationReplayStore replay_store{};
  AllowAuthorizer authorizer{};
  FakeWorkflow workflow{};
  CalibrationTransportController controller(workflow, 0x1234U, &authorizer,
                                             &replay_store);
  CalibrationResultEnvelope result{};
  auto start = command(CalibrationOperation::Start,
                       CalibrationTransport::Https, 80U);
  TEST_ASSERT_TRUE(controller.execute(start, 1U, result));
  auto commit = command(CalibrationOperation::Commit,
                        CalibrationTransport::Https, 81U, result.session_id);
  workflow.proposal.present = true;
  workflow.proposal.version = 1U;
  commit.expected_profile_present = true;
  commit.expected_profile = workflow.proposal;
  replay_store.fail_save = true;
  TEST_ASSERT_FALSE(controller.execute(commit, 2U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Failed),
                        static_cast<int>(result.status));
  TEST_ASSERT_EQUAL_UINT32(1U, workflow.commit_calls);

  replay_store.fail_save = false;
  CalibrationResultEnvelope replayed{};
  TEST_ASSERT_TRUE(controller.execute(commit, 3U, replayed));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Replayed),
                        static_cast<int>(replayed.status));
  TEST_ASSERT_EQUAL_UINT32(1U, workflow.commit_calls);
  TEST_ASSERT_TRUE(replayed.profile.present);
}

void test_failed_prepare_does_not_activate_profile() {
  MemoryCalibrationReplayStore replay_store{};
  replay_store.fail_prepare = true;
  AllowAuthorizer authorizer{};
  FakeWorkflow workflow{};
  CalibrationTransportController controller(workflow, 0x1234U, &authorizer,
                                             &replay_store);
  CalibrationResultEnvelope result{};
  auto start = command(CalibrationOperation::Start,
                       CalibrationTransport::Https, 82U);
  TEST_ASSERT_TRUE(controller.execute(start, 1U, result));
  auto commit = command(CalibrationOperation::Commit,
                        CalibrationTransport::Https, 83U, result.session_id);
  TEST_ASSERT_FALSE(controller.execute(commit, 2U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Failed),
                        static_cast<int>(result.status));
  TEST_ASSERT_EQUAL_UINT32(0U, workflow.commit_calls);
}

void test_prepared_intent_cannot_be_replaced_by_another_request() {
  MemoryCalibrationReplayStore replay_store{};
  const auto first = command(CalibrationOperation::Commit,
                             CalibrationTransport::Https, 84U, 9U);
  auto second = first;
  second.request_id = 85U;
  TEST_ASSERT_TRUE(replay_store.prepare(first));
  TEST_ASSERT_FALSE(replay_store.prepare(second));
}

void test_replay_still_requires_current_authorization() {
  MemoryCalibrationReplayStore replay_store{};
  ToggleAuthorizer authorizer{};
  FakeWorkflow workflow{};
  CalibrationTransportController controller(workflow, 0x1234U, &authorizer,
                                             &replay_store);
  CalibrationResultEnvelope result{};
  const auto start = command(CalibrationOperation::Start,
                             CalibrationTransport::Https, 86U);
  TEST_ASSERT_TRUE(controller.execute(start, 1U, result));
  authorizer.allowed = false;
  TEST_ASSERT_FALSE(controller.execute(start, 2U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Unauthorized),
                        static_cast<int>(result.status));
}

void test_committed_replay_survives_controller_recreation() {
  MemoryCalibrationReplayStore replay_store{};
  AllowAuthorizer authorizer{};
  FakeWorkflow first_workflow{};
  CalibrationTransportController first(first_workflow, 0x1234U, &authorizer,
                                       &replay_store);
  CalibrationResultEnvelope result{};
  auto start = command(CalibrationOperation::Start,
                       CalibrationTransport::Https, 70U);
  TEST_ASSERT_TRUE(first.execute(start, 1U, result));
  auto commit = command(CalibrationOperation::Commit,
                        CalibrationTransport::Https, 71U, result.session_id);
  first_workflow.proposal.present = true;
  first_workflow.proposal.version = 2U;
  first_workflow.proposal.hash[0] = 0x5AU;
  commit.expected_profile_present = true;
  commit.expected_profile = first_workflow.proposal;
  TEST_ASSERT_TRUE(first.execute(commit, 2U, result));

  FakeWorkflow restarted_workflow{};
  CalibrationTransportController restarted(restarted_workflow, 0x1234U,
                                            &authorizer, &replay_store);
  CalibrationResultEnvelope replayed{};
  TEST_ASSERT_TRUE(restarted.execute(commit, 3U, replayed));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Replayed),
                        static_cast<int>(replayed.status));
  TEST_ASSERT_TRUE(replayed.profile.present);
  TEST_ASSERT_EQUAL_UINT32(0U, restarted_workflow.start_calls);

  commit.payload_size = 1U;
  commit.payload_digest[0] = 0xFFU;
  TEST_ASSERT_FALSE(restarted.execute(commit, 4U, replayed));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Conflict),
                        static_cast<int>(replayed.status));
}

void test_commit_returns_terminal_status_and_profile_identity() {
  FakeWorkflow workflow{};
  AllowAuthorizer authorizer{};
  CalibrationTransportController controller(workflow, 0x1234U, &authorizer);
  CalibrationResultEnvelope result{};
  auto start = command(CalibrationOperation::Start,
                       CalibrationTransport::Https, 50U);
  TEST_ASSERT_TRUE(controller.execute(start, 1U, result));
  auto commit = command(CalibrationOperation::Commit,
                        CalibrationTransport::Https, 51U, result.session_id);
  workflow.proposal.present = true;
  workflow.proposal.version = 2U;
  workflow.proposal.hash[0] = 0xA5U;
  commit.expected_profile_present = true;
  commit.expected_profile = workflow.proposal;
  TEST_ASSERT_TRUE(controller.execute(commit, 2U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Committed),
                        static_cast<int>(result.status));
  TEST_ASSERT_TRUE(result.profile.present);
  TEST_ASSERT_EQUAL_UINT32(1U, result.profile.version);
}

void test_controller_observes_committed_profile_without_replaying_transition() {
  FakeWorkflow workflow{};
  AllowAuthorizer authorizer{};
  EventSink sink{};
  CalibrationTransportController controller(workflow, 0x1234U, &authorizer);
  controller.set_debug_sink(&sink);
  CalibrationResultEnvelope result{};
  auto start = command(CalibrationOperation::Start,
                       CalibrationTransport::Https, 501U);
  TEST_ASSERT_TRUE(controller.execute(start, 1U, result));
  auto commit = command(CalibrationOperation::Commit,
                        CalibrationTransport::Https, 502U, result.session_id);
  workflow.proposal.present = true;
  workflow.proposal.version = 2U;
  commit.expected_profile_present = true;
  commit.expected_profile = workflow.proposal;
  sink.count = 0U;
  TEST_ASSERT_TRUE(controller.execute(commit, 2U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Committed),
                        static_cast<int>(result.status));
  TEST_ASSERT_EQUAL_UINT32(3U, sink.count);
  TEST_ASSERT_EQUAL_UINT32(50U, sink.events[0].event_code);
  TEST_ASSERT_EQUAL_UINT32(52U, sink.events[1].event_code);
  TEST_ASSERT_EQUAL_UINT32(51U, sink.events[2].event_code);
  TEST_ASSERT_EQUAL_INT(0, sink.events[1].arg0);
  TEST_ASSERT_EQUAL_INT(10, sink.events[1].arg1);

  sink.count = 0U;
  TEST_ASSERT_TRUE(controller.execute(commit, 3U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Replayed),
                        static_cast<int>(result.status));
  TEST_ASSERT_EQUAL_UINT32(1U, sink.count);
  TEST_ASSERT_EQUAL_UINT32(50U, sink.events[0].event_code);
}

void test_request_id_reuse_with_changed_payload_digest_is_conflict() {
  FakeWorkflow workflow{};
  AllowAuthorizer authorizer{};
  CalibrationTransportController controller(workflow, 0x1234U, &authorizer);
  CalibrationResultEnvelope result{};
  auto start = command(CalibrationOperation::Start, CalibrationTransport::Ble, 60U);
  start.payload_size = 3U;
  start.payload_digest[0] = 0x11U;
  TEST_ASSERT_TRUE(controller.execute(start, 1U, result));
  start.payload_digest[0] = 0x22U;
  TEST_ASSERT_FALSE(controller.execute(start, 2U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Conflict),
                        static_cast<int>(result.status));
  TEST_ASSERT_EQUAL_UINT32(1U, workflow.start_calls);
}

void test_generic_workflow_failure_does_not_claim_internal_cause() {
  FakeWorkflow workflow{};
  AllowAuthorizer authorizer{};
  EventSink sink{};
  CalibrationTransportController controller(workflow, 0x1234U, &authorizer);
  controller.set_debug_sink(&sink);
  CalibrationResultEnvelope result{};
  auto start = command(CalibrationOperation::Start, CalibrationTransport::Usb, 90U);
  workflow.status_.active = true;  // workflow rejects this start; no typed cause exists.
  TEST_ASSERT_FALSE(controller.execute(start, 100U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Failed),
                        static_cast<int>(result.status));
  TEST_ASSERT_EQUAL_UINT32(1U, sink.count);
  TEST_ASSERT_EQUAL_INT(0, sink.events[0].arg1);
}

void test_owner_events_report_acquire_and_release_without_countdown_noise() {
  FakeWorkflow workflow{};
  EventSink sink{};
  CalibrationTransportController controller(workflow, 0x1234U);
  controller.set_debug_sink(&sink);
  CalibrationResultEnvelope result{};
  auto start = command(CalibrationOperation::Start, CalibrationTransport::Usb, 91U);
  TEST_ASSERT_TRUE(controller.execute(start, 10U, result));
  TEST_ASSERT_EQUAL_UINT32(2U, sink.count);
  TEST_ASSERT_EQUAL_UINT32(50U, sink.events[0].event_code);
  TEST_ASSERT_EQUAL_UINT32(51U, sink.events[1].event_code);
  TEST_ASSERT_EQUAL_INT(1, sink.events[1].arg0);
  TEST_ASSERT_EQUAL_INT(300000, sink.events[1].arg1);
  auto cancel = command(CalibrationOperation::Cancel, CalibrationTransport::Usb,
                        92U, result.session_id);
  TEST_ASSERT_TRUE(controller.execute(cancel, 20U, result));
  TEST_ASSERT_EQUAL_UINT32(4U, sink.count);
  TEST_ASSERT_EQUAL_UINT32(51U, sink.events[3].event_code);
  TEST_ASSERT_EQUAL_INT(0, sink.events[3].arg0);
  TEST_ASSERT_EQUAL_INT(0, sink.events[3].arg1);
}

void test_controller_observes_results_without_changing_replay_or_owner() {
  FakeWorkflow workflow{};
  AllowAuthorizer authorizer{};
  EventSink sink{};
  CalibrationTransportController controller(workflow, 0x1234U, &authorizer);
  controller.set_debug_sink(&sink);
  CalibrationResultEnvelope result{};
  auto start = command(CalibrationOperation::Start, CalibrationTransport::Ble,
                       UINT32_MAX, 0U);
  TEST_ASSERT_TRUE(controller.execute(start, 100U, result));
  TEST_ASSERT_EQUAL_UINT32(2U, sink.count);
  TEST_ASSERT_EQUAL_UINT32(50U, sink.events[0].event_code);
  TEST_ASSERT_EQUAL_UINT32(51U, sink.events[1].event_code);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, sink.events[0].request_id);
  TEST_ASSERT_EQUAL_UINT32(sigap::debug::kHasRequestId, sink.events[0].presence);
  TEST_ASSERT_TRUE(controller.active());

  TEST_ASSERT_TRUE(controller.execute(start, 101U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Replayed),
                        static_cast<int>(result.status));
  TEST_ASSERT_EQUAL_UINT32(3U, sink.count);
  TEST_ASSERT_EQUAL_INT(2, sink.events[1].arg0);

  auto too_wide = command(CalibrationOperation::GetStatus,
                          CalibrationTransport::Ble,
                          static_cast<uint64_t>(UINT32_MAX) + 1U);
  TEST_ASSERT_TRUE(controller.execute(too_wide, 102U, result));
  TEST_ASSERT_EQUAL_UINT32(0U, sink.events[3].request_id);
  TEST_ASSERT_EQUAL_UINT32(0U, sink.events[3].presence);
  TEST_ASSERT_TRUE(controller.active());
}

void test_failed_start_replay_remains_failed_without_second_workflow_call() {
  FakeWorkflow workflow{};
  workflow.start_allowed = false;
  AllowAuthorizer authorizer{};
  CalibrationTransportController controller(workflow, 0x1234U, &authorizer);
  const auto start = command(CalibrationOperation::Start, CalibrationTransport::Https, 901U);
  CalibrationResultEnvelope result{};
  TEST_ASSERT_FALSE(controller.execute(start, 10U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Failed), static_cast<int>(result.status));
  TEST_ASSERT_FALSE(controller.execute(start, 11U, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Failed), static_cast<int>(result.status));
  TEST_ASSERT_EQUAL_UINT32(1U, workflow.start_attempts);
  TEST_ASSERT_EQUAL_UINT32(0U, workflow.start_calls);
  TEST_ASSERT_FALSE(controller.active());
}

void test_persisted_failed_replay_is_not_promoted_after_controller_recreation() {
  FakeWorkflow workflow{};
  AllowAuthorizer authorizer{};
  MemoryCalibrationReplayStore store{};
  const auto start = command(CalibrationOperation::Start, CalibrationTransport::Https, 902U);
  CalibrationResultEnvelope failed{};
  failed.request_id = start.request_id;
  failed.status = CalibrationCommandStatus::Failed;
  TEST_ASSERT_TRUE(store.save(start, failed));
  for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
    CalibrationTransportController controller(workflow, 0x1234U, &authorizer, &store);
    CalibrationResultEnvelope result{};
    TEST_ASSERT_FALSE(controller.execute(start, 10U + attempt, result));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationCommandStatus::Failed), static_cast<int>(result.status));
    TEST_ASSERT_FALSE(controller.active());
  }
  TEST_ASSERT_EQUAL_UINT32(0U, workflow.start_attempts);
}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_owner_and_idempotent_start);
  RUN_TEST(test_failed_start_replay_remains_failed_without_second_workflow_call);
  RUN_TEST(test_persisted_failed_replay_is_not_promoted_after_controller_recreation);
  RUN_TEST(test_session_owner_binds_authenticated_principal);
  RUN_TEST(test_expiry_cancels_workflow_and_disconnect_is_local);
  RUN_TEST(test_expected_profile_conflict_and_preview);
  RUN_TEST(test_remote_transport_requires_authorization);
  RUN_TEST(test_expiry_and_device_binding_are_rejected);
  RUN_TEST(test_local_deadline_is_not_replay_identity);
  RUN_TEST(test_failed_replay_persistence_is_reconciled_without_second_commit);
  RUN_TEST(test_failed_prepare_does_not_activate_profile);
  RUN_TEST(test_prepared_intent_cannot_be_replaced_by_another_request);
  RUN_TEST(test_replay_still_requires_current_authorization);
  RUN_TEST(test_commit_returns_terminal_status_and_profile_identity);
  RUN_TEST(test_controller_observes_committed_profile_without_replaying_transition);
  RUN_TEST(test_request_id_reuse_with_changed_payload_digest_is_conflict);
  RUN_TEST(test_generic_workflow_failure_does_not_claim_internal_cause);
  RUN_TEST(test_owner_events_report_acquire_and_release_without_countdown_noise);
  RUN_TEST(test_committed_replay_survives_controller_recreation);
  RUN_TEST(test_controller_observes_results_without_changing_replay_or_owner);
  UNITY_END();
}

void loop() {}

int main() {
  setup();
  return 0;
}
