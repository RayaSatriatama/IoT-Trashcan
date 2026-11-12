#include "sigap_app.h"
#include "test_platform.h"
#include "fixtures.h"
#include "model_fixtures.h"
#include <cstdio>
#include <ctime>
#include <cstring>

namespace sigap::board { void log_runtime_board_memory() noexcept {} }
extern "C" std::time_t time(std::time_t* output) noexcept {
  const auto result = static_cast<std::time_t>(test_platform::wall_seconds);
  if (output) *output = result;
  return result;
}

namespace sigap {
namespace tp = test_platform;
struct ForwardCounter final : acquisition::ISampleObserver {
  explicit ForwardCounter(acquisition::ISampleObserver& downstream) : downstream(downstream) {}
  void observe(const acquisition::SampleRecord& value) noexcept override {
    ++calls;
    downstream.observe(value);
  }
  acquisition::ISampleObserver& downstream;
  unsigned calls{};
};
struct CountingStore final : storage::IArchiveStore {
  unsigned calls{};
  storage::MemoryArchiveStore<8> memory{1024U * 1024U};
  storage::AppendStatus append(const storage::ImmutableRecordBytes& value) noexcept override {
    ++calls; return memory.append(value);
  }
  storage::AppendStatus append_gap(const storage::GapRange& value) noexcept override {
    ++calls; return memory.append_gap(value);
  }
  bool checkpoint() noexcept override { ++calls; return true; }
  bool recover() noexcept override { ++calls; return true; }
  uint64_t pending_bytes() const noexcept override { return memory.pending_bytes(); }
  uint64_t pending_quota_bytes() const noexcept override { return memory.pending_quota_bytes(); }
  storage::ArchiveHealth health() const noexcept override { return memory.health(); }
  bool peek_pending(storage::ImmutableRecordBytes& value) noexcept override {
    ++calls; return memory.peek_pending(value);
  }
};

struct DebugAppProbe {
  using Owner = debug::IoOwner;
  using Kind = maintenance::CommandKind;
  static void pump() {
    unsigned budget = 10;
    while (!tp::tasks.empty()) { assert(budget--); tp::run_one_task(); }
  }
  static void emit(SigapApp& app) {
    const auto sequence = app.boot_identity_.sequence.next_value();
    debug::EventInput event{};
    event.monotonic_us = tp::now_us;
    event.module = debug::Module::Runtime;
    event.event_code = 1;
    assert(app.debug_service_.emit(event));
    assert(sequence == app.boot_identity_.sequence.next_value());
    const auto meta = app.debug_service_.meta(tp::now_us);
    assert(meta.boot.present && meta.boot.bytes == app.boot_identity_.boot_id.bytes);
  }
  static void start(SigapApp& app, bool force_overlapping_cleanup = false) {
    assert(app.debug_worker_.has_value());
    emit(app);
    app.loop();
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
    assert(app.io_gate_.active(Owner::ControlHttp));
    assert(!app.debug_worker_->transport_pending());
    assert(tp::control_starts > 0);
    assert(tp::archive_starts > 0);
    assert(app.io_gate_.active(Owner::ArchiveHttp));
    pump();
    app.loop();
#endif
    if (force_overlapping_cleanup) {
      // Adversarial precondition only: normal app admission forbids this overlap.
      // Prime the REAL worker/task, not a fake gate, to verify real loop expiry.
      assert(app.calibration_controller_.active());
      assert(tp::debug_starts == 0);
      app.debug_worker_->poll(tp::now_us, {true, true, true, true, false, false});
    }
    assert(app.io_gate_.active(Owner::DebugHttp));
    assert(app.debug_worker_->transport_pending());
    assert(!app.debug_transport_->quiescent());
    assert(tp::debug_starts == 1);
  }
  static void retire(SigapApp& app) {
    app.debug_worker_->cancel();
    pump();
    app.loop();
    assert(!app.io_gate_.active(Owner::DebugHttp));
    assert(!app.debug_worker_->transport_pending());
    app.loop();
    pump();
    app.loop();
    assert(tp::tasks.empty());
  }
  static maintenance::CommandStatus command(SigapApp& app, Kind kind,
                                             const char* target = "", bool must_be_busy = false) {
    maintenance::MaintenanceCommand command{};
    command.kind = kind;
    assert(command.target.assign(target));
    maintenance::MaintenanceResponse result{};
    const auto status = app.maintenance_router_.execute(command, tp::now_us, result);
    if (must_be_busy) assert(result.view().find("ERR STORAGE_BUSY") != std::string_view::npos);
    return status;
  }
  static void denied(SigapApp& app) {
    const auto mode = app.maintenance_router_.mode();
    const auto state = app.config_manager_.state();
    const auto nvs = tp::nvs_calls;
    const auto fs = tp::fs_calls;
    const auto probe = tp::sd_begins;
    for (const auto kind : {Kind::SecretStatus, Kind::ConfigDraftStart,
                           Kind::ProfileStatus, Kind::StorageSdTest}) {
      const char* target = "hx711_organic";
      assert(command(app, kind, target, true) == maintenance::CommandStatus::Rejected);
    }
    // Accepted C1 contract: model status snapshots RAM, unlike secret status.
    assert(command(app, Kind::ModelStatus, "safety_b3") == maintenance::CommandStatus::Accepted);
    maintenance::MaintenanceCommand model{};
    model.kind = Kind::ModelRollback;
    assert(model.target.assign("safety_b3"));
    maintenance::MaintenanceResponse response{};
    assert(app.model_router_.execute_model(model, tp::now_us, response) ==
           maintenance::CommandStatus::Rejected);
    assert(response.view().find("ERR STORAGE_BUSY") != std::string_view::npos);
    char text[512]{}; size_t size{};
    const auto active = app.calibration_controller_.active();
    assert(!app.ble_calibration_backend_.execute("CAL START hx711_organic", text, sizeof(text), size));
    assert(app.calibration_controller_.active() == active);
    assert(!app.wifi_backend_.request_connect("TEST-NEXT", "TEST-PASSWORD"));
    assert(!app.wifi_backend_.retry_wifi());
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
    network::DeviceControlCommand control{};
    assert(control.command_id.assign("TEST-COMMAND"));
    assert(control.device_id.assign(app.device_control_id_.data()));
    assert(control.operator_id.assign("TEST-OPERATOR"));
    assert(control.session_id.assign("TEST-SESSION"));
    assert(control.sensor_id.assign("hx711_organic"));
    assert(control.operation.assign("START"));
    assert(control.request_hash.assign(std::string(64U, 'a')));
    control.payload[0] = '{'; control.payload[1] = '}'; control.payload_size = 2;
    control.expires_at_us = tp::now_us + 1000000U;
    network::DeviceControlResult control_result{};
    assert(!app.device_control_executor_.execute(control, tp::now_us, control_result));
    assert(app.calibration_controller_.active() == active);
#endif
    assert(app.config_manager_.state() == state);
    assert(app.maintenance_router_.mode() == mode);
    assert(tp::nvs_calls == nvs && tp::fs_calls == fs && tp::sd_begins == probe);
  }
  static acquisition::SampleRecord make_record(SigapApp& app, uint64_t at,
                                                acquisition::SampleQuality quality,
                                                acquisition::SampleReason reason) {
    acquisition::SampleRecord record{};
    acquisition::RawObservation raw{};
    acquisition::ConvertedObservation converted{};
    acquisition::ProfileRef profile{};
    assert(app.record_factory_.make(at, acquisition::SensorId::Hx711Organic,
        acquisition::StreamId::Organic, acquisition::SampleKind::Hx711Counts,
        quality, reason, raw, converted, profile, record) ==
        acquisition::DriverPollStatus::Produced);
    return record;
  }
  static void observe_record(SigapApp& app, const acquisition::SampleRecord& record) {
    app.processing_runtime_.observe(record);
  }
  static calibration::DiagnosticEvidence organic_evidence(SigapApp& app) {
    calibration::DiagnosticEvidence evidence{};
    assert(app.evidence_cache_.diagnostic(calibration::CalibrationTarget::Hx711Organic,
                                          evidence));
    assert(evidence.available);
    return evidence;
  }
  static void bound_observer_case(SigapApp& app, const std::string& scenario) {
    const auto base_sequence = app.boot_identity_.sequence.next_value();
    const auto record = make_record(app, tp::now_us, acquisition::SampleQuality::Uncalibrated,
                                    acquisition::SampleReason::ProfileMissing);
    const auto allocated_sequence = app.boot_identity_.sequence.next_value();
    assert(allocated_sequence == base_sequence + 1U);
    if (scenario == "filter") assert(app.debug_service_.set_level(debug::Level::Error));
    if (scenario == "full") {
      assert(app.debug_service_.set_level(debug::Level::Debug));
      for (unsigned index = 0; index < 64U; ++index) {
        debug::EventInput event{};
        event.monotonic_us = tp::now_us;
        event.module = debug::Module::Runtime;
        event.event_code = 1;
        assert(app.debug_service_.emit(event));
      }
    }
    const auto before_meta = app.debug_service_.meta(tp::now_us);
    const auto source_bytes = [&] {
      std::array<uint8_t, sizeof(record)> bytes{};
      std::memcpy(bytes.data(), &record, sizeof(record));
      return bytes;
    }();
    if (scenario == "ownership") {
      assert(app.acquisition_.handoff().try_push(record) == acquisition::HandoffStatus::Enqueued);
      assert(app.acquisition_.handoff().size() > 0U);
      assert(app.acquisition_.handoff().dropped_records() == 0U);
      acquisition::SampleRecord pending{};
      assert(app.acquisition_.handoff().try_peek(pending) == acquisition::HandoffStatus::Dequeued);
      std::array<uint8_t, sizeof(pending)> prefix_bytes{};
      std::memcpy(prefix_bytes.data(), &pending, sizeof(pending));
      assert(pending.header.sequence == record.header.sequence);
      const auto ring_size = app.acquisition_.handoff().size();
      const auto dropped = app.acquisition_.handoff().dropped_records();
      const auto ownership_sequence = app.boot_identity_.sequence.next_value();
      const auto assert_ownership_unchanged = [&] {
        assert(app.boot_identity_.sequence.next_value() == ownership_sequence);
        assert(app.acquisition_.handoff().size() == ring_size && ring_size > 0U);
        assert(app.acquisition_.handoff().dropped_records() == dropped);
        acquisition::SampleRecord current{};
        assert(app.acquisition_.handoff().try_peek(current) == acquisition::HandoffStatus::Dequeued);
        std::array<uint8_t, sizeof(current)> current_bytes{};
        std::memcpy(current_bytes.data(), &current, sizeof(current));
        assert(current_bytes == prefix_bytes);
        assert(source_bytes == [&] {
          std::array<uint8_t, sizeof(record)> bytes{};
          std::memcpy(bytes.data(), &record, sizeof(record));
          return bytes;
        }());
      };
      observe_record(app, record);
      assert_ownership_unchanged();
      const auto ownership_evidence = organic_evidence(app);
      const auto ownership_event = app.debug_service_.read({app.boot_identity_.boot_id, before_meta.latest});
      assert(ownership_event.has_event && ownership_event.event.module == debug::Module::Acquisition);
      assert(ownership_event.event.related_sequence == record.header.sequence);
      assert(ownership_evidence.observed_at_us == record.header.monotonic_us);
      const auto cursor = app.debug_service_.meta(tp::now_us).latest;
      (void)app.debug_service_.read({app.boot_identity_.boot_id, cursor});
      debug::BatchSnapshot batch{};
      (void)app.debug_service_.capture({app.boot_identity_.boot_id, cursor}, 8U, tp::now_us, batch);
      assert_ownership_unchanged();
      assert(organic_evidence(app).observed_at_us == record.header.monotonic_us);
      return;
    }
    observe_record(app, record);
    const auto evidence = organic_evidence(app);
    assert(evidence.quality == record.quality && evidence.reason == record.reason);
    assert(evidence.observed_at_us == record.header.monotonic_us);
    assert(app.boot_identity_.sequence.next_value() == allocated_sequence);
    const auto after_meta = app.debug_service_.meta(tp::now_us);
    if (scenario == "filter") assert(after_meta.latest == before_meta.latest);
    else {
      assert(after_meta.latest > before_meta.latest);
      auto page = app.debug_service_.read({app.boot_identity_.boot_id, before_meta.latest});
      assert(page.has_event && page.event.module == debug::Module::Acquisition);
      assert(page.event.related_sequence == record.header.sequence);
    }
    if (scenario == "full") {
      assert(after_meta.overwritten == before_meta.overwritten + 1U);
      assert(after_meta.latest == before_meta.latest + 1U);
      auto event = app.debug_service_.read({app.boot_identity_.boot_id, before_meta.latest});
      assert(event.has_event && event.event.event_code == 40U);
      assert(event.event.level == debug::Level::Info);
      assert((event.event.presence & debug::kHasRelatedSequence) != 0U);
      assert(event.event.related_sequence == record.header.sequence);
      assert(event.event.monotonic_us == record.header.monotonic_us);
    }
    if (scenario == "coalesced") {
      const auto first_latest = after_meta.latest;
      const auto next = make_record(app, tp::now_us + 1U, record.quality, record.reason);
      observe_record(app, next);
      assert(organic_evidence(app).observed_at_us == next.header.monotonic_us);
      assert(app.debug_service_.meta(tp::now_us + 1U).latest == first_latest);
    }
    if (scenario == "routine") {
      auto before = app.debug_service_.meta(tp::now_us).latest;
      for (unsigned i = 0; i < 3U; ++i) {
        (void)app.debug_service_.meta(tp::now_us);
        (void)app.debug_service_.read({app.boot_identity_.boot_id, before});
        debug::BatchSnapshot batch{};
        (void)app.debug_service_.capture({app.boot_identity_.boot_id, before}, 8U,
                                         tp::now_us, batch);
      }
      assert(app.debug_service_.meta(tp::now_us).latest == before);
    }
  }
  static void loop_gap_business_case(SigapApp& app, const std::string& scenario) {
    const bool ble_case = scenario.rfind("ble_chain", 0U) == 0U;
    const bool filtered = scenario == "loopgap_business_filtered" || scenario == "ble_chain_filtered";
    const bool full = scenario == "loopgap_full" || scenario == "ble_chain_full";
    assert(app.debug_service_.set_level(filtered ? debug::Level::Error : debug::Level::Debug));
    if (full) {
      debug::EventInput filler{};
      filler.module = debug::Module::Runtime;
      filler.event_code = 1U;
      for (unsigned i = 0U; i < 64U; ++i) assert(app.debug_service_.emit(filler));
    }
    tp::now_us = 0U;
    const auto sample = make_record(app, 0U, acquisition::SampleQuality::Uncalibrated,
                                    acquisition::SampleReason::ProfileMissing);
    std::array<uint8_t, sizeof(sample)> source_bytes{};
    std::memcpy(source_bytes.data(), &sample, sizeof(sample));
    auto& samples = app.acquisition_.handoff();
    assert(samples.try_push(sample) == acquisition::HandoffStatus::Enqueued);
    const auto sample_prefix = [&] {
      acquisition::SampleRecord value{};
      assert(samples.try_peek(value) == acquisition::HandoffStatus::Dequeued);
      assert(value.header.sequence == sample.header.sequence);
      std::array<uint8_t, sizeof(value)> bytes{};
      std::memcpy(bytes.data(), &value, sizeof(value));
      return bytes;
    };
    const auto sample_before = sample_prefix();
    auto& archive = app.archive_runtime_.storage_worker().queue();
    const auto archive_size = archive.size();
    const auto* archive_prefix = archive.peek();
    assert(archive_size > 0U && archive_prefix != nullptr);
    std::array<uint8_t, sizeof(*archive_prefix)> archive_bytes{};
    std::memcpy(archive_bytes.data(), archive_prefix, archive_bytes.size());
    // Hold the real gate so the normal loop must preserve nonempty logger prefixes.
    assert(app.io_gate_.try_acquire(Owner::Filesystem));
    const auto sequence = app.boot_identity_.sequence.next_value();
    const auto observed = app.processing_runtime_.observed_samples();
    const auto cycles = app.processing_runtime_.cycles_completed();
    const auto before = app.debug_service_.meta(tp::now_us);
    if (ble_case) {
      app.provisioning_clock_.set_now(tp::now_us);
      app.ble_session_.on_connect(tp::now_us);
      provisioning::BleResponse response{};
      assert(app.ble_session_.receive("SCAN_WIFI", response) == provisioning::BleParseStatus::Accepted);
      assert(app.boot_identity_.sequence.next_value() == sequence);
      assert(sample_prefix() == sample_before && archive.peek() == archive_prefix);
      const auto after_command = app.debug_service_.meta(tp::now_us);
      unsigned links{}, starts{}, rejections{};
      for (auto cursor = before.latest; cursor < after_command.latest;) {
        const auto page = app.debug_service_.read({after_command.boot, cursor});
        assert(page.has_event);
        cursor = page.event.sequence;
        if (page.event.module != debug::Module::Ble) continue;
        assert(page.event.presence == 0U && page.event.request_id == 0U);
        assert(page.event.monotonic_us == tp::now_us);
        if (page.event.event_code == 10U) {
          assert(page.event.arg0 == 0 && page.event.arg1 == 0);
          ++links;
        } else if (page.event.event_code == 12U) {
          assert(page.event.arg0 == 1 && page.event.arg1 == 9);
          ++starts;
        } else if (page.event.event_code == 15U) {
          assert(page.event.arg0 == 0 && page.event.arg1 == 0);
          ++rejections;
        } else {
          assert(false);  // Rendering alone cannot create notify-admission progress.
        }
      }
      assert(links == (filtered ? 0U : 1U));
      assert(starts == (filtered ? 0U : 1U));
      assert(rejections == (filtered ? 0U : 1U));
      debug::BatchSnapshot batch{};
      (void)app.debug_service_.capture({after_command.boot, after_command.latest}, 8U, tp::now_us, batch);
      assert(app.debug_service_.meta(tp::now_us).latest == after_command.latest);
      assert(app.boot_identity_.sequence.next_value() == sequence);
      assert(archive.size() == archive_size && archive.peek() == archive_prefix);
      assert(std::memcmp(archive_prefix, archive_bytes.data(), archive_bytes.size()) == 0);
      assert(sample_prefix() == sample_before);
    }
    app.loop();
    size_t health_events{};
    const auto first = app.debug_service_.meta(tp::now_us);
    for (auto cursor = before.latest; cursor < first.latest;) {
      const auto page = app.debug_service_.read({first.boot, cursor});
      assert(page.has_event);
      cursor = page.event.sequence;
      if (page.event.module == debug::Module::Runtime && page.event.event_code == 3U) {
        assert(page.boot.bytes == app.boot_identity_.boot_id.bytes);
        assert(page.event.arg0 == 0 && page.event.arg1 == 0);
        assert(page.event.level == debug::Level::Debug && page.event.presence == 0U);
        ++health_events;
      }
    }
    assert(health_events == (filtered ? 0U : 1U));
    if (full) assert(first.overwritten > before.overwritten);
    for (unsigned i = 0U; i < 3U; ++i) {
      tp::now_us += 1000000U;
      app.loop();
      assert(app.io_gate_.active(Owner::Filesystem));
      assert(!app.io_gate_.active(Owner::DebugHttp));
      assert(!app.io_gate_.active(Owner::ControlHttp));
      assert(!app.io_gate_.active(Owner::ArchiveHttp));
      assert(archive.size() == archive_size && archive.peek() == archive_prefix);
      assert(std::memcmp(archive_prefix, archive_bytes.data(), archive_bytes.size()) == 0);
      assert(sample_prefix() == sample_before);
      assert(std::memcmp(&sample, source_bytes.data(), source_bytes.size()) == 0);
    }
    assert(app.acquisition_.enabled() && app.processing_runtime_.enabled());
    assert(app.boot_identity_.sequence.next_value() > sequence);
    assert(app.processing_runtime_.observed_samples() > observed);
    assert(app.processing_runtime_.cycles_completed() > cycles);
    // Three periods under the held logger gate deliberately exceed the 16-slot handoff.
    assert(samples.size() == acquisition::AcquisitionFoundation::kHandoffCapacity);
    assert(samples.dropped_records() > 0U);
    assert(tp::tasks.empty() && tp::http_calls == 0U);
    std::printf("%s sequence=%llu observed=%llu cycles=%llu samples=%zu dropped=%llu "
                "results=%zu archive=%zu nvs_calls=%zu nvs_writes=%zu fs_calls=%zu\n",
        ble_case ? "BLE_EFFECTS" : "LOOPGAP_EFFECTS",
        static_cast<unsigned long long>(app.boot_identity_.sequence.next_value()),
        static_cast<unsigned long long>(app.processing_runtime_.observed_samples()),
        static_cast<unsigned long long>(app.processing_runtime_.cycles_completed()),
        samples.size(), static_cast<unsigned long long>(samples.dropped_records()),
        app.processing_runtime_.handoff().size(), archive.size(),
        tp::nvs_calls, tp::nvs_writes, tp::fs_calls);
    app.io_gate_.release(Owner::Filesystem);
    assert(!app.io_gate_.active(Owner::Filesystem));
  }
  static void model53_transitions(SigapApp& app) {
    const std::array<models::ModelProfileKey, 4U> keys{{
        {acquisition::ProfileKind::Fis, models::ModelTarget::FisOrganic},
        {acquisition::ProfileKind::Fis, models::ModelTarget::FisInorganic},
        {acquisition::ProfileKind::Fis, models::ModelTarget::FisB3},
        {acquisition::ProfileKind::Safety, models::ModelTarget::SafetyB3}}};
    const auto& codec = models::ModelProfilePersistence::default_codec();
    const auto install = [&](std::size_t index, uint32_t version) {
      models::EncodedModelProfile encoded{};
      if (index == 3U) {
        assert(codec.encode(debug_model_fixture::test_safety(version), encoded));
      } else {
        auto value = debug_model_fixture::test_fis(version);
        value.manifest.target_id = static_cast<uint32_t>(keys[index].target);
        assert(codec.encode(value, encoded));
      }
      assert(app.model_lifecycle_.stage(keys[index], encoded));
      assert(app.model_lifecycle_.validate(keys[index]));
      assert(app.model_lifecycle_.preview(keys[index]));
      assert(app.model_lifecycle_.commit(keys[index]));
      assert(app.model_lifecycle_.state(keys[index]) == models::ModelLifecycleState::Active);
    };
    const auto observe = [&](uint8_t changed_mask, int32_t state) {
      std::array<models::ModelProfileSnapshot, 4U> before{};
      for (std::size_t i = 0U; i < keys.size(); ++i)
        assert(app.model_lifecycle_.snapshot(keys[i], before[i]));
      const auto meta = app.debug_service_.meta(tp::now_us);
      const auto sequence = app.boot_identity_.sequence.next_value();
      const auto profile_generation = app.profile_manager_.generation();
      const auto samples = app.acquisition_.handoff().size();
      const auto results = app.processing_runtime_.handoff().size();
      const auto archive = app.archive_runtime_.storage_worker().queue().size();
      const auto nvs = tp::nvs_calls, writes = tp::nvs_writes, fs = tp::fs_calls;
      const auto http = tp::http_calls, tasks = tp::tasks.size();
      app.refresh_model_profiles();
      assert(tp::nvs_calls == nvs && tp::nvs_writes == writes && tp::fs_calls == fs);
      assert(tp::http_calls == http && tp::tasks.size() == tasks);
      assert(app.boot_identity_.sequence.next_value() == sequence);
      assert(app.profile_manager_.generation() == profile_generation);
      assert(app.acquisition_.handoff().size() == samples);
      assert(app.processing_runtime_.handoff().size() == results);
      assert(app.archive_runtime_.storage_worker().queue().size() == archive);
      auto cursor = meta.latest;
      for (std::size_t i = 0U; i < keys.size(); ++i) {
        models::ModelProfileSnapshot after{};
        assert(app.model_lifecycle_.snapshot(keys[i], after));
        assert(before[i].present == after.present);
        assert(before[i].ref.profile_version == after.ref.profile_version);
        assert(before[i].encoded.size == after.encoded.size &&
               before[i].encoded.bytes == after.encoded.bytes);
        if ((changed_mask & (1U << i)) == 0U) continue;
        const auto page = app.debug_service_.read({meta.boot, cursor});
        assert(page.has_event && page.event.sequence == cursor + 1U);
        assert(page.event.module == debug::Module::Calibration && page.event.event_code == 53U);
        assert(page.event.level == debug::Level::Info && page.event.monotonic_us == tp::now_us);
        assert(page.event.arg0 == state);
        const auto version = after.ref.profile_version;
        assert(page.event.arg1 == (version > static_cast<uint32_t>(INT32_MAX)
                   ? INT32_MAX : static_cast<int32_t>(version)));
        cursor = page.event.sequence;
        std::printf("MODEL53 key=%zu state=%d version=%u observed=%d\n",
                    i, state, version, page.event.arg1);
      }
      assert(app.debug_service_.meta(tp::now_us).latest == cursor);
    };
    observe(0U, 1);  // Setup already observed four unavailable states.
    for (std::size_t i = 0U; i < keys.size(); ++i) install(i, static_cast<uint32_t>(7U + i));
    observe(15U, 0);
    assert(app.fis_model_present_ && app.safety_model_present_);
    observe(0U, 0);
    auto& entries = models::model53_entries(app.model_lifecycle_);
    for (std::size_t i = 0U; i < keys.size(); ++i) {
      const auto valid = entries[i].active;
      entries[i].active.bytes[0] ^= 1U;  // Explicit host RAM-corruption fault, not a public workflow.
      models::ModelProfileSnapshot corrupted{};
      assert(app.model_lifecycle_.snapshot(keys[i], corrupted) && corrupted.present);
      assert(!codec.validate(corrupted.encoded, keys[i]));
      observe(static_cast<uint8_t>(1U << i), 2);
      observe(0U, 2);
      if (i == 3U) assert(!app.safety_model_present_);
      entries[i].active = valid;
      observe(static_cast<uint8_t>(1U << i), 0);
      observe(0U, 0);
    }
    // Same state, actual version transition, with uint32 -> int32 saturation.
    for (std::size_t i = 0U; i < keys.size(); ++i) install(i, UINT32_MAX);
    observe(15U, 0);
    observe(0U, 0);
    assert(app.fis_model_present_ && app.safety_model_present_);
    std::puts("MODEL53_TRANSITIONS PASS keys=4 events=16");
  }
  static void run(const std::string& scenario) {
    seed_test_boot(scenario != "credential");
    if (scenario == "startup_config_failure") test_platform::nvs.clear();
    SigapApp app;
    const bool startup_filter = scenario == "startup_filter";
    const bool startup_full = scenario == "startup_full";
    if (startup_filter) assert(app.debug_service_.set_level(debug::Level::Error));
    if (startup_full) {
      assert(app.debug_service_.set_level(debug::Level::Debug));
      for (unsigned index = 0; index < 64U; ++index) {
        debug::EventInput filler{};
        filler.module = debug::Module::Runtime;
        filler.event_code = 1U;
        assert(app.debug_service_.emit(filler));
      }
    }
    const auto startup_before = app.debug_service_.meta(tp::now_us);
    const auto wifi_before = startup_before;
    const auto setup_started_us = tp::now_us;
    const auto science_before = app.boot_identity_.sequence.next_value();
    const bool startup_case = scenario.rfind("startup_", 0U) == 0U;
    bool config_read_observed{};
    if (startup_case) {
      tp::on_nvs_read = [&](const std::string& name_space) {
        if (name_space != "sigap_cfg_meta" || config_read_observed) return;
        config_read_observed = true;
        if (startup_filter) return;
        const auto during_config = app.debug_service_.meta(tp::now_us);
        bool configuration_begin_visible{};
        for (auto cursor = startup_before.latest; cursor < during_config.latest;) {
          const auto page = app.debug_service_.read({during_config.boot, cursor});
          assert(page.has_event);
          cursor = page.event.sequence;
          configuration_begin_visible = configuration_begin_visible ||
              (page.event.module == debug::Module::Runtime && page.event.event_code == 2U &&
               page.event.arg0 == 0 && page.event.arg1 == 2);
        }
        assert(configuration_begin_visible);
      };
    }
    app.setup();
    tp::on_nvs_read = {};
    const auto setup_finished_us = tp::now_us;
    if (startup_case) assert(config_read_observed);
    const auto startup_effects = [&] {
      std::printf("STARTUP_EFFECTS sequence=%llu samples=%zu results=%zu archive=%zu "
                  "nvs_calls=%zu nvs_writes=%zu fs_calls=%zu\n",
          static_cast<unsigned long long>(app.boot_identity_.sequence.next_value()),
          app.acquisition_.handoff().size(), app.processing_runtime_.handoff().size(),
          app.archive_runtime_.storage_worker().queue().size(),
          tp::nvs_calls, tp::nvs_writes, tp::fs_calls);
    };
    if (scenario == "default_level") {
#ifdef SIGAP_FULL_DEBUG_BUILD
      constexpr auto expected = debug::Level::Debug;
      constexpr const char* expected_name = "DEBUG";
#else
      constexpr auto expected = debug::Level::Info;
      constexpr const char* expected_name = "INFO";
#endif
      assert(app.debug_service_.meta(tp::now_us).level == expected);
      const auto changed = expected == debug::Level::Info
                               ? debug::Level::Debug : debug::Level::Info;
      assert(app.debug_service_.set_level(changed));
      assert(app.debug_service_.meta(tp::now_us).level == changed);
      SigapApp reconstructed{};
      assert(reconstructed.debug_service_.meta(tp::now_us).level == expected);
      std::printf("DEFAULT_LEVEL %s PASS\n", expected_name);
      return;
    }
    if (scenario == "model53_transitions") {
      model53_transitions(app);
      return;
    }
    if (scenario == "composition_routes") {
      assert(app.debug_service_.meta(tp::now_us).level == debug::Level::Info);
      auto model_meta = app.debug_service_.meta(tp::now_us);
      unsigned unavailable_models{};
      auto model_cursor = model_meta.oldest == 0U ? 0U : model_meta.oldest - 1U;
      while (model_cursor < model_meta.latest) {
        const auto page = app.debug_service_.read({model_meta.boot, model_cursor});
        assert(page.has_event);
        model_cursor = page.event.sequence;
        if (page.event.module == debug::Module::Calibration &&
            page.event.event_code == 53U) {
          assert(page.event.arg0 == 1 && page.event.arg1 == 0);
          ++unavailable_models;
        }
      }
      assert(unavailable_models == 4U);
      app.refresh_model_profiles();
      assert(app.debug_service_.meta(tp::now_us).latest == model_meta.latest);

      maintenance::MaintenanceCommand usb_meta{};
      usb_meta.kind = maintenance::CommandKind::DebugMeta;
      maintenance::MaintenanceResponse usb_response{};
      assert(app.maintenance_router_.execute(usb_meta, tp::now_us, usb_response) ==
             maintenance::CommandStatus::Accepted);
      assert(usb_response.view().find("SIGAP_DEBUG DBG1|M|") != std::string_view::npos);

      app.provisioning_clock_.set_now(tp::now_us);
      app.ble_session_.on_connect(tp::now_us);
      provisioning::BleResponse ble_response{};
      const auto boot = app.debug_service_.meta(tp::now_us).boot;
      char boot_text[33]{};
      constexpr char hex[] = "0123456789abcdef";
      for (std::size_t i = 0U; i < boot.bytes.size(); ++i) {
        boot_text[i * 2U] = hex[boot.bytes[i] >> 4U];
        boot_text[i * 2U + 1U] = hex[boot.bytes[i] & 15U];
      }
      const auto cursor = app.debug_service_.meta(tp::now_us).latest;
      std::string page_command{"GET_DEBUG PAGE "};
      page_command.append(boot_text, 32U);
      page_command.append(" 0");
      assert(app.ble_session_.receive(page_command, ble_response) ==
             provisioning::BleParseStatus::Accepted);
      assert(ble_response.notify);
      assert(std::string_view(ble_response.bytes.data(), ble_response.size)
                 .find("DBG1|P|") != std::string_view::npos);
      assert(app.debug_service_.meta(tp::now_us).latest == cursor);
      const auto after_page = app.debug_service_.meta(tp::now_us).latest;
      assert(app.ble_session_.receive("GET_DEBUG META", ble_response) ==
             provisioning::BleParseStatus::Accepted);
      assert(ble_response.notify);
      assert(std::string_view(ble_response.bytes.data(), ble_response.size)
                 .find("DBG1|M|") != std::string_view::npos);
      assert(app.debug_service_.meta(tp::now_us).latest == after_page);
      assert(app.ble_session_.receive("SET_DEBUG_LEVEL DEBUG", ble_response) ==
             provisioning::BleParseStatus::Accepted);
      assert(app.debug_service_.meta(tp::now_us).level == debug::Level::Debug);
      assert(std::string_view(ble_response.bytes.data(), ble_response.size)
                 .find("DBG1|L|3") != std::string_view::npos);

      tp::serial_output.clear();
      tp::serial_writable = 0U;
      const auto sequence_before = app.boot_identity_.sequence.next_value();
      app.loop();
      assert(tp::serial_output.empty());
      tp::serial_writable = 4096U;
      app.loop();
      assert(!tp::serial_output.empty());
      assert(tp::serial_output.size() <= 384U);
      assert(tp::serial_output.find("SIGAP_DEBUG DBG1|P|") != std::string::npos);
      assert(app.boot_identity_.sequence.next_value() > sequence_before);
      std::printf("COMPOSITION_ROUTES PASS\n");
      return;
    }
    const auto validate_startup_event = [&](const debug::Page& page,
                                           uint64_t previous_us) {
      assert(page.boot.present && page.boot.bytes == app.boot_identity_.boot_id.bytes);
      assert(page.boot.bytes == startup_before.boot.bytes);
      const auto& event = page.event;
      assert(event.level == debug::Level::Info && event.presence == 0U);
      assert(event.request_id == 0U && event.related_sequence == 0U && event.duration_us == 0U);
      assert(event.monotonic_us >= setup_started_us && event.monotonic_us >= previous_us);
      assert(event.monotonic_us <= setup_finished_us);
    };
    const std::array<uint16_t, 6U> expected_codes{{1U, 2U, 2U, 2U, 2U, 1U}};
    const std::array<int32_t, 6U> expected_states{{0, 0, 1, 0, 1, 1}};
    const std::array<int32_t, 6U> expected_stages{{1, 2, 2, 3, 3, 1}};
    if (startup_filter || startup_full) {
      const auto after = app.debug_service_.meta(tp::now_us);
      if (startup_filter) {
        assert(after.latest == startup_before.latest);
      } else {
        assert(after.overwritten > startup_before.overwritten);
        size_t startup_events{};
        auto previous_us = setup_started_us;
        auto cursor = after.oldest > startup_before.latest + 1U
                          ? after.oldest - 1U : startup_before.latest;
        while (cursor < after.latest) {
          const auto page = app.debug_service_.read({after.boot, cursor});
          assert(page.has_event);
          cursor = page.event.sequence;
          if (page.event.module == debug::Module::Runtime &&
              (page.event.event_code == 1U || page.event.event_code == 2U)) {
            validate_startup_event(page, previous_us);
            previous_us = page.event.monotonic_us;
            assert(startup_events < expected_codes.size());
            assert(page.event.event_code == expected_codes[startup_events]);
            assert(page.event.arg0 == expected_states[startup_events]);
            assert(page.event.arg1 == expected_stages[startup_events]);
            ++startup_events;
          }
        }
        assert(startup_events == 6U);
      }
      assert(app.config_manager().runtime_ready() && app.acquisition().enabled());
      assert(app.processing_runtime_.enabled());
      startup_effects();
      return;
    }
    if (scenario == "startup_positive" || scenario == "startup_config_failure") {
      const auto startup_after = app.debug_service_.meta(tp::now_us);
      std::array<debug::EventRecord, 8> events{};
      size_t count{};
      auto previous_us = setup_started_us;
      auto cursor = startup_before.latest;
      while (cursor < startup_after.latest) {
        const auto page = app.debug_service_.read({startup_before.boot, cursor});
        assert(page.has_event);
        cursor = page.event.sequence;
        if (page.event.module == debug::Module::Runtime &&
            (page.event.event_code == 1U || page.event.event_code == 2U)) {
          validate_startup_event(page, previous_us);
          previous_us = page.event.monotonic_us;
          assert(count < events.size());
          events[count++] = page.event;
        }
      }
      const bool failed = scenario == "startup_config_failure";
      assert(count == (failed ? 4U : 6U));
      assert(events[0].event_code == 1U && events[0].arg0 == 0 && events[0].arg1 == 1);
      assert(events[1].event_code == 2U && events[1].arg0 == 0 && events[1].arg1 == 2);
      assert(events[2].event_code == 2U && events[2].arg1 == 2 &&
             events[2].arg0 == (failed ? 2 : 1));
      assert(events[3].event_code == (failed ? 1U : 2U));
      if (failed) {
        assert(events[3].arg0 == 2 && events[3].arg1 == 2);
        assert(!app.config_manager().runtime_ready());
        assert(!app.acquisition().enabled());
        assert(!app.processing_runtime_.enabled());
        assert(app.processing_runtime_.cycles_completed() == 0U);
        assert(app.processing_runtime_.observed_samples() == 0U);
        assert(app.boot_identity_.sequence.next_value() == science_before);
        assert(app.processing_runtime_.handoff().size() == 0U);
        assert(app.acquisition_.handoff().size() == 0U);
        assert(app.archive_runtime_.storage_worker().queue().size() == 0U);
      } else {
        assert(events[3].arg0 == 0 && events[3].arg1 == 3);
        assert(events[4].event_code == 2U && events[4].arg0 == 1 && events[4].arg1 == 3);
        assert(events[5].event_code == 1U && events[5].arg0 == 1 && events[5].arg1 == 1);
        assert(app.config_manager().runtime_ready() && app.acquisition().enabled());
        assert(app.processing_runtime_.enabled());
      }
      const auto science_after_setup = app.boot_identity_.sequence.next_value();
      const auto queue_size = app.archive_runtime_.storage_worker().queue().size();
      const auto* queue_prefix = app.archive_runtime_.storage_worker().queue().peek();
      if (!failed) assert(queue_size > 0U && queue_prefix != nullptr);
      for (unsigned i = 0U; i < 3U; ++i) {
        (void)app.debug_service_.meta(tp::now_us);
        (void)app.debug_service_.read({startup_after.boot, startup_after.latest});
        debug::BatchSnapshot batch{};
        (void)app.debug_service_.capture({startup_after.boot, startup_after.latest},
                                        8U, tp::now_us, batch);
      }
      assert(app.debug_service_.meta(tp::now_us).latest == startup_after.latest);
      assert(app.boot_identity_.sequence.next_value() == science_after_setup);
      assert(app.archive_runtime_.storage_worker().queue().size() == queue_size);
      assert(app.archive_runtime_.storage_worker().queue().peek() == queue_prefix);
      startup_effects();
      return;
    }
    assert(app.config_manager().runtime_ready());
    assert(app.acquisition().enabled());
    if (scenario.rfind("ble_chain", 0U) == 0U ||
        scenario == "loopgap_full" || scenario == "loopgap_business_debug" ||
        scenario == "loopgap_business_filtered") {
      loop_gap_business_case(app, scenario);
      return;
    }
    if (scenario.rfind("loopgap_", 0U) == 0U) {
      const bool filtered_case = scenario == "loopgap_filter";
      assert(app.debug_service_.set_level(filtered_case ? debug::Level::Error : debug::Level::Debug));
      const int64_t initial_time = scenario == "loopgap_regression" ? 100 : 0;
      tp::now_us = initial_time;
      const auto before_first = app.debug_service_.meta(tp::now_us);
      app.loop();
      auto meta = app.debug_service_.meta(tp::now_us);
      const auto find_health = [&](uint64_t cursor) {
        debug::Page found{};
        const auto end = app.debug_service_.meta(tp::now_us).latest;
        while (cursor < end) {
          const auto item = app.debug_service_.read({meta.boot, cursor});
          assert(item.has_event);
          cursor = item.event.sequence;
          if (item.event.module == debug::Module::Runtime && item.event.event_code == 3U) {
            found = item;
            break;
          }
        }
        return found;
      };
      const auto health_count = [&](uint64_t cursor) {
        size_t count{};
        const auto end = app.debug_service_.meta(tp::now_us).latest;
        while (cursor < end) {
          const auto item = app.debug_service_.read({meta.boot, cursor});
          assert(item.has_event);
          cursor = item.event.sequence;
          if (item.event.module == debug::Module::Runtime && item.event.event_code == 3U) ++count;
        }
        return count;
      };
      const auto first = find_health(before_first.latest);
      if (filtered_case) {
        assert(!first.has_event);
        const auto filtered_cursor = meta.latest;
        tp::now_us = 100U;
        app.loop();
        assert(!find_health(filtered_cursor).has_event);
        assert(app.debug_service_.set_level(debug::Level::Debug));
        tp::now_us = 200U;
        app.loop();
        assert(health_count(filtered_cursor) == 0U);
        tp::now_us = 1000201U;
        app.loop();
        const auto changed = find_health(filtered_cursor);
        assert(changed.has_event && changed.event.arg0 == 1 && changed.event.arg1 == 1000001);
        assert(health_count(filtered_cursor) == 1U);
        tp::now_us += 1000001U;
        app.loop();
        assert(health_count(filtered_cursor) == 1U);
        return;
      }
      assert(first.has_event && first.event.arg0 == 0);
      assert(first.event.arg1 == 0 && first.event.monotonic_us == static_cast<uint64_t>(initial_time));
      assert(first.event.presence == 0U && first.event.request_id == 0U &&
             first.event.related_sequence == 0U && first.event.duration_us == 0U);
      meta = app.debug_service_.meta(0U);
      const auto baseline = meta.latest;
      const auto baseline_health_count = health_count(baseline);
      if (scenario == "loopgap_regression") {
        tp::now_us = 1000101U;
        app.loop();
        const auto late = find_health(baseline);
        assert(late.has_event && late.event.arg0 == 1 && late.event.arg1 == 1000001);
        assert(health_count(baseline) == 1U);
        tp::now_us = 50;
        app.loop();
        assert(health_count(baseline) == 1U);
        tp::now_us = 1000051U;
        app.loop();
        assert(health_count(baseline) == 1U);
        tp::now_us += 1U;
        app.loop();
        const auto after_rebase = find_health(late.event.sequence);
        assert(after_rebase.has_event && after_rebase.event.arg0 == 0 &&
               after_rebase.event.arg1 == 1);
        assert(health_count(baseline) == 2U);
        return;
      }
      if (scenario == "loopgap_passive") {
        tp::now_us = 6000001;
        assert(health_count(baseline) == baseline_health_count);
        return;
      }
      uint64_t gap = scenario == "loopgap_healthy" ? 1000000U :
                     scenario == "loopgap_late" ? 1000001U :
                     scenario == "loopgap_late_max" ? 5000000U :
                     scenario == "loopgap_stalled" ? 5000001U :
                     scenario == "loopgap_int32max" ? 2147483647ULL :
                     scenario == "loopgap_saturated" ? 2147483648ULL : 100U;
      tp::now_us = static_cast<int64_t>(gap);
      app.loop();
      meta = app.debug_service_.meta(gap);
      if (gap <= 1000000U) {
        assert(health_count(baseline) == baseline_health_count);
      } else {
        const auto page = find_health(baseline);
        assert(page.has_event);
        assert(page.event.arg0 == (gap <= 5000000U ? 1 : 2));
        assert(page.event.arg1 == (gap > static_cast<uint64_t>(INT32_MAX) ? INT32_MAX : static_cast<int32_t>(gap)));
        assert(page.event.monotonic_us == gap && page.event.presence == 0U);
      }
      const auto after_transition = app.debug_service_.meta(gap).latest;
      const auto emitted_after_transition = health_count(baseline);
      tp::now_us += static_cast<int64_t>(gap);
      app.loop();
      assert(health_count(baseline) == emitted_after_transition);
      if (scenario == "loopgap_recovery") {
        tp::now_us += 5000001;
        app.loop();
        auto recovered = find_health(after_transition);
        assert(recovered.has_event && recovered.event.arg0 == 2);
        const auto stalled_cursor = recovered.event.sequence;
        tp::now_us += 1;
        app.loop();
        recovered = find_health(stalled_cursor);
        assert(recovered.has_event && recovered.event.arg0 == 0 && recovered.event.arg1 == 1);
      }
      return;
    }
    if (scenario.rfind("storage_chain", 0U) == 0U) {
      const bool filtered = scenario == "storage_chain_filtered";
      const bool full = scenario == "storage_chain_full";
      if (filtered) assert(app.debug_service_.set_level(debug::Level::Error));
      const auto overwrite_before = app.debug_service_.meta(tp::now_us).overwritten;
      if (full) {
        assert(app.debug_service_.set_level(debug::Level::Debug));
        for (unsigned index = 0U; index < 64U; ++index) {
          debug::EventInput event{};
          event.monotonic_us = tp::now_us;
          event.module = debug::Module::Runtime;
          event.event_code = 1U;
          assert(app.debug_service_.emit(event));
        }
      }
      const auto before = wifi_before.latest;
      const auto operation_cursor = app.debug_service_.meta(tp::now_us).latest;
      storage::MemoryArchiveStore<8U> archive(256U * 1024U);
      app.archive_runtime_.set_store(&archive);
      app.batch_runtime_.bind(&archive, &app.batch_store_, &app.ack_journal_);
      assert(app.archive_runtime_.poll_storage() == storage::AppendStatus::Durable);
      assert(app.batch_runtime_.poll() == telemetry_archive::BatchRuntimeStatus::Durable);
      telemetry_archive::Ack ack{};
      const auto& batch = app.batch_runtime_.batch();
      ack.batch_id_size = batch.batch_id_size;
      std::memcpy(ack.batch_id.data(), batch.batch_id.data(), batch.batch_id_size);
      ack.body_hash = batch.body_hash;
      ack.record_count = batch.record_count;
      ack.stored_count = batch.record_count;
      ack.device_id = batch.device_id;
      ack.status = telemetry_archive::AckStatus::Durable;
      assert(app.batch_runtime_.apply_ack(ack) == telemetry_archive::AckPersistStatus::Durable);
      assert(app.archive_runtime_.append_sample(
                 make_record(app, tp::now_us + 1U, acquisition::SampleQuality::Uncalibrated,
                             acquisition::SampleReason::ProfileMissing)) ==
             storage::AppendStatus::Queued);
      assert(app.archive_runtime_.poll_storage() == storage::AppendStatus::Durable);
      assert(app.batch_runtime_.poll() == telemetry_archive::BatchRuntimeStatus::Durable);
      const auto& sample_batch = app.batch_runtime_.batch();
      ack = telemetry_archive::Ack{};
      ack.batch_id_size = sample_batch.batch_id_size;
      std::memcpy(ack.batch_id.data(), sample_batch.batch_id.data(), sample_batch.batch_id_size);
      ack.body_hash = sample_batch.body_hash;
      ack.record_count = sample_batch.record_count;
      ack.stored_count = sample_batch.record_count;
      ack.device_id = sample_batch.device_id;
      ack.status = telemetry_archive::AckStatus::Durable;
      assert(app.batch_runtime_.apply_ack(ack) == telemetry_archive::AckPersistStatus::Durable);
      const auto end = app.debug_service_.meta(tp::now_us).latest;
      if (filtered) {
        assert(end == operation_cursor);
        assert(archive.size() == 0U);
        return;
      }
      bool stored{}, acked{}, reclaimed{}, queue_busy{}, queue_empty{};
      const auto oldest = app.debug_service_.meta(tp::now_us).oldest;
      for (uint64_t cursor = full && oldest != 0U ? oldest - 1U : before;
           cursor < end;) {
        const auto page = app.debug_service_.read({app.boot_identity_.boot_id, cursor});
        assert(page.has_event);
        cursor = page.event.sequence;
        if (page.event.module == debug::Module::Storage && page.event.event_code == 31U) {
          queue_busy = queue_busy || page.event.arg0 == 1;
          queue_empty = queue_empty || page.event.arg0 == 0;
        }
        if (page.event.module == debug::Module::Storage && page.event.event_code == 32U) {
          stored = stored || page.event.arg0 == 0;
          acked = acked || page.event.arg0 == 1;
          reclaimed = reclaimed || page.event.arg0 == 2;
        }
      }
      assert(queue_busy && queue_empty && stored && acked && reclaimed);
      if (full) {
        const auto full_meta = app.debug_service_.meta(tp::now_us);
        assert(full_meta.overwritten > overwrite_before);
      }
      assert(archive.size() == 0U);
      return;
    }
    if (scenario == "wifi_scan") {
      auto cursor = wifi_before.latest;
      bool connected_event{};
      const auto setup_end = app.debug_service_.meta(tp::now_us).latest;
      while (cursor < setup_end) {
        const auto page = app.debug_service_.read({wifi_before.boot, cursor});
        assert(page.has_event);
        cursor = page.event.sequence;
        if (page.event.module == debug::Module::Wifi && page.event.event_code == 25U) {
          connected_event = true;
          break;
        }
      }
      assert(connected_event);
      const auto scan_cursor = app.debug_service_.meta(tp::now_us).latest;
      assert(app.wifi_backend_.request_scan() == provisioning::WifiScanStatus::Failed);
      const auto failure = app.debug_service_.read({wifi_before.boot, scan_cursor});
      assert(failure.has_event && failure.event.module == debug::Module::Wifi);
      assert(failure.event.event_code == 24U);
      assert(failure.event.level == debug::Level::Info);
      assert(failure.event.arg0 == 0 && failure.event.arg1 == 0);
      return;
    }
    if (scenario.rfind("processing_chain", 0U) == 0U) {
      const bool filtered = scenario == "processing_chain_filtered";
      const bool full = scenario == "processing_chain_full";
      assert(app.debug_service_.set_level(filtered ? debug::Level::Error :
                                                   debug::Level::Debug));
      if (full) {
        debug::EventInput filler{};
        filler.module = debug::Module::Runtime;
        filler.event_code = 1U;
        filler.monotonic_us = tp::now_us;
        for (unsigned i = 0U; i < 64U; ++i) assert(app.debug_service_.emit(filler));
      }
      const auto before = app.debug_service_.meta(tp::now_us);
      const auto sequence = app.boot_identity_.sequence.next_value();
      for (unsigned i = 0U; i < 3U; ++i) {
        tp::now_us += 1000000U;
        app.loop();
      }
      assert(app.acquisition_.enabled() && app.processing_runtime_.enabled());
      assert(app.boot_identity_.sequence.next_value() > sequence);
      assert(app.processing_runtime_.observed_samples() > 0U);
      assert(app.processing_runtime_.cycles_completed() > 0U);
      const auto after = app.debug_service_.meta(tp::now_us);
      if (filtered) {
        assert(after.latest == before.latest);
        return;
      }
      bool counters{}, availability{};
      auto cursor = before.latest;
      if (after.oldest > cursor + 1U) cursor = after.oldest - 1U;
      while (cursor < after.latest) {
        const auto page = app.debug_service_.read({after.boot, cursor});
        assert(page.has_event && page.event.sequence > cursor);
        cursor = page.event.sequence;
        if (page.event.module != debug::Module::Acquisition) continue;
        if (page.event.event_code == 41U) {
          assert(page.event.arg0 == 0 || page.event.arg0 == 1);
          assert(page.event.arg1 > 0);
          assert(page.event.level == debug::Level::Debug);
          assert(page.event.presence == 0U);
          counters = true;
        }
        if (page.event.event_code == 42U) {
          assert(page.event.arg0 >= 0 && page.event.arg0 <= 2);
          assert(page.event.arg1 >= 0 && page.event.arg1 <= 8);
          assert(page.event.presence == 0U);
          availability = true;
        }
      }
      assert(counters && availability);
      if (full) assert(after.overwritten > before.overwritten);
      assert(app.debug_service_.meta(tp::now_us).latest == after.latest);
      return;
    }
    if (scenario == "acquisition_loop") {
      const auto sequence_before = app.boot_identity_.sequence.next_value();
      const auto debug_before = app.debug_service_.meta(tp::now_us).latest;
      tp::now_us += 100000U;
      app.loop();
      assert(app.boot_identity_.sequence.next_value() > sequence_before);
      calibration::DiagnosticEvidence evidence{};
      bool observed = false;
      for (unsigned target = 0; target < 13U; ++target) {
        if (app.evidence_cache_.diagnostic(
                static_cast<calibration::CalibrationTarget>(target), evidence) &&
            evidence.available) observed = true;
      }
      assert(observed);
      const auto after_loop = app.debug_service_.meta(tp::now_us);
      assert(after_loop.latest > debug_before);
      for (auto cursor = debug_before; cursor < after_loop.latest;) {
        const auto page = app.debug_service_.read({after_loop.boot, cursor});
        assert(page.has_event);
        cursor = page.event.sequence;
        assert(page.event.module != debug::Module::Runtime ||
               (page.event.event_code != 1U && page.event.event_code != 2U));
      }
      return;
    }
    if (scenario == "positive" || scenario == "filter" || scenario == "full" ||
        scenario == "coalesced" || scenario == "ownership" || scenario == "routine") {
      bound_observer_case(app, scenario);
      return;
    }
    ForwardCounter processing_evidence{app.runtime_debug_observer_};
    app.processing_runtime_.set_downstream_observer(&processing_evidence);
    if (scenario == "unavailable" || scenario == "credential") {
      assert(!app.debug_worker_.has_value());
      emit(app); app.loop(); pump(); app.loop(); pump();
      assert(!app.io_gate_.active(Owner::DebugHttp));
      assert(tp::debug_starts == 0);
      return;
    }
    if (scenario == "control_time_loss") {
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
      app.loop();
      assert(app.io_gate_.active(Owner::ControlHttp));
      tp::wall_seconds = 0;
      pump();
      app.loop();
      assert(!app.io_gate_.active(Owner::ControlHttp));
      assert(!app.io_gate_.active(Owner::DebugHttp));
      assert(tp::debug_starts == 0);
      const auto starts = tp::tasks.size();
      assert(app.io_gate_.try_acquire(Owner::ControlHttp));
      network::DeviceControlHttpResponse response{};
      assert(app.control_transport_boundary_.start_next_command(
          app.device_control_id_.data(), "TEST-DEVICE-TOKEN", 1000, response) ==
          network::TransportResult::Failed);
      assert(tp::tasks.size() == starts);
      app.io_gate_.release(Owner::ControlHttp);
#endif
      return;
    }
    if (scenario == "time" || scenario == "wifi") {
      if (scenario == "time") tp::wall_seconds = 0;
      if (scenario == "wifi") tp::wifi_up = false;
      emit(app); app.loop(); pump(); app.loop(); pump();
      assert(tp::debug_starts == 0);
      assert(!app.io_gate_.active(Owner::DebugHttp));
      assert(app.acquisition().enabled());
      return;
    }
    if (scenario == "response" || scenario == "cancel" || scenario == "wifi_loss" ||
        scenario == "expiry" || scenario == "calibration_priority") {
      assert(command(app, Kind::MaintenanceEnter) == maintenance::CommandStatus::Accepted);
    }
    if (scenario == "expiry" || scenario == "calibration_priority") {
      char response[512]{}; size_t size{};
      assert(app.ble_calibration_backend_.execute("CAL START hx711_organic", response, sizeof(response), size));
      assert(app.calibration_controller_.active());
    }
    if (scenario == "calibration_priority") {
      emit(app); app.loop(); pump(); app.loop();
      assert(app.calibration_controller_.active());
      assert(tp::debug_starts == 0);
      assert(!app.io_gate_.active(Owner::DebugHttp));
      return;
    }
    start(app, scenario == "expiry");
    if (scenario == "priority") { retire(app); return; }

    CountingStore store, origin, destination;
    storage::ArchiveStoreMigrationSource source{origin, storage::ArchiveMedia::LittleFs};
    storage::ArchiveStoreMigrationDestination sink{&destination, storage::ArchiveMedia::Sd};
    storage::ArchiveMediaMigration migration{source, sink, storage::ArchiveMedia::LittleFs,
        storage::ArchiveMedia::Sd, storage::MigrationBudget{}, nullptr};
    app.archive_runtime_.set_store(&store);
    app.archive_runtime_.set_media_migration(&migration);
    tp::reset_io();
    const auto starts = tp::tasks.size();
    const auto debug_starts = tp::debug_starts;
    const auto archive_starts = tp::archive_starts;
    const auto control_starts = tp::control_starts;
    const auto http_calls = tp::http_calls;
    const auto untouched = [&] {
      assert(app.io_gate_.active(Owner::DebugHttp));
      assert(tp::fs_calls == 0 && tp::nvs_calls == 0);
      assert(store.calls == 0 && origin.calls == 0 && destination.calls == 0);
      assert(tp::debug_starts == debug_starts);
      assert(tp::archive_starts == archive_starts && tp::control_starts == control_starts);
    };
    denied(app);
    for (int i = 0; i < 3; ++i) { app.loop(); untouched(); }
    assert(tp::tasks.size() == starts);
    assert(tp::http_calls == http_calls);
    if (scenario == "wifi_loss") tp::wifi_up = false;
    if (scenario == "expiry") {
      const auto sequence = app.boot_identity_.sequence.next_value();
      const auto observed = processing_evidence.calls;
      tp::now_us += 100000U;
      app.loop(); untouched();
      assert(app.boot_identity_.sequence.next_value() > sequence);
      assert(processing_evidence.calls > observed);
      acquisition::SampleRecord pending{};
      assert(app.acquisition_.handoff().try_peek(pending) == acquisition::HandoffStatus::Dequeued);
      const auto retained = pending.header.sequence;
      tp::now_us += calibration::CalibrationSession::kSessionTimeoutUs + 1U;
      app.loop(); untouched();
      assert(!app.calibration_controller_.active());
      assert(app.acquisition_.handoff().try_peek(pending) == acquisition::HandoffStatus::Dequeued);
      assert(pending.header.sequence == retained);
    }
    if (scenario == "response") {
      unsigned ended{}, destroyed{};
      tp::on_http_end = [&] {
        ++ended;
        assert(!app.debug_transport_->quiescent());
        app.loop(); untouched();
        app.debug_worker_->cancel();
        app.loop(); untouched();
        assert(std::string_view(tp::borrowed_body, tp::borrowed_size) == tp::last_debug_body);
      };
      tp::on_tls_destroy = [&] {
        ++destroyed;
        assert(!app.debug_transport_->quiescent());
        app.loop(); untouched();
        assert(std::string_view(tp::borrowed_body, tp::borrowed_size) == tp::last_debug_body);
      };
      pump();
      assert(ended == 1 && destroyed == 1);
      assert(app.debug_transport_->quiescent());
      untouched();
      tp::on_http_end = {}; tp::on_tls_destroy = {};
      assert(tp::last_debug_url == build::kDebugUploadConfig.endpoint);
      assert(tp::last_debug_body.find("\"device_id\":\"TEST-DEVICE\"") != std::string::npos);
      std::string boot;
      constexpr char hex[] = "0123456789abcdef";
      for (auto byte : app.boot_identity_.boot_id.bytes) {
        boot += hex[byte >> 4U]; boot += hex[byte & 15U];
      }
      assert(tp::last_debug_body.find("\"boot_id\":\"" + boot + "\"") != std::string::npos);
    } else {
      app.debug_worker_->cancel();
      for (int i = 0; i < 3; ++i) { app.loop(); untouched(); }
      pump();
      assert(app.debug_transport_->quiescent());
      untouched();
    }
    app.loop();
    assert(!app.io_gate_.active(Owner::DebugHttp));
    assert(!app.debug_worker_->transport_pending());
    assert(tp::fs_calls == 0 && tp::nvs_calls == 0 && store.calls == 0);
    app.loop();
    assert(store.calls > 0 && origin.calls > 0);
    const auto reads = tp::nvs_calls;
    assert(command(app, Kind::SecretStatus) == maintenance::CommandStatus::Accepted);
    assert(tp::nvs_calls > reads);
    assert(command(app, Kind::ConfigDraftStart) == maintenance::CommandStatus::Accepted);
    assert(command(app, Kind::ConfigCancel) == maintenance::CommandStatus::Accepted);
    assert(command(app, Kind::ProfileStatus, "hx711_organic") == maintenance::CommandStatus::Accepted);
    assert(command(app, Kind::MaintenanceEnter) == maintenance::CommandStatus::Accepted);
    char calibration_response[512]{}; size_t calibration_size{};
    assert(app.ble_calibration_backend_.execute("CAL START hx711_organic", calibration_response,
        sizeof(calibration_response), calibration_size));
    assert(app.calibration_controller_.active());
    assert(app.ble_calibration_backend_.execute("CAL CANCEL hx711_organic", calibration_response,
        sizeof(calibration_response), calibration_size));
    assert(!app.calibration_controller_.active());
    const auto probe_calls = tp::sd_begins;
    (void)command(app, Kind::StorageSdTest);
    assert(tp::sd_begins == probe_calls + 1U);
    tp::wifi_up = true;
    const auto writes = tp::nvs_writes;
    assert(app.wifi_backend_.request_connect("TEST-NEXT", "TEST-PASSWORD"));
    app.loop();
    assert(tp::nvs_writes == writes + 1U);
    app.loop();
    assert(tp::nvs_writes == writes + 1U);
    pump(); app.loop(); pump();
    assert(tp::tasks.empty());
  }
};
}
int main(int argc, char** argv) {
  assert(argc == 2);
  sigap::DebugAppProbe::run(argv[1]);
  std::printf("CASE %s PASS\n", argv[1]);
}
