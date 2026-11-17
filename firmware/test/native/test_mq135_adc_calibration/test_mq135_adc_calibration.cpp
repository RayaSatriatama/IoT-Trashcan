// S1 focused regression: same-sample calibrated ADC voltage evidence carried
// through MqAdcReadResult -> MqAdcAcquirer -> RawObservation.
// No SDK call happens here: the decision step is driven with SDK-shaped probe
// results, so an unsupported eFuse can never become 0 mV or a 3300 mV default.
#include <cmath>
#include <cstdio>
#include <limits>
#include <string_view>

#include <unity.h>

#include "acquisition/mq_adc_calibration.h"
#include "acquisition/record.h"
#include "acquisition/sensors/mq_adc.h"
#include "storage/archive_codec.h"
#include "telemetry_archive/ack_journal_store.h"
#include "telemetry_archive/batch_store.h"
#include "telemetry_archive/runtime.h"

using namespace sigap::acquisition;

void setUp() {}
void tearDown() {}

namespace {

MqAdcCalibrationProbe supported_probe(uint32_t mv) {
  MqAdcCalibrationProbe probe{};
  probe.efuse_tp_fit_supported = true;
  probe.characterized_tp_fit = true;
  probe.converted = true;
  probe.pin_mv = mv;
  return probe;
}

struct MqFake final : IMqAdcReader {
  MqAdcReadResult result{true, false, 2700U, SampleReason::DataReady};
  int reads{0};
  MqAdcReadResult read() noexcept override {
    ++reads;
    return result;
  }
};

MqAdcProfile profile_for(ProfileTarget target) {
  MqAdcProfile profile{};
  profile.manifest.kind = ProfileKind::Adc;
  profile.manifest.validity = ProfileValidity::Valid;
  profile.manifest.target_id = static_cast<uint32_t>(target);
  profile.manifest.profile_version = 1U;
  profile.reference_mv = 3000U;
  profile.full_scale_counts = 3000U;
  profile.divider_numerator = 3U;
  profile.divider_denominator = 5U;
  return profile;
}

RecordContext context() {
  RecordContext record_context{};
  record_context.boot_id.present = true;
  record_context.boot_id.bytes[0] = 0xa5U;
  record_context.device_id.present = true;
  record_context.device_id.bytes[0] = 0x11U;
  // TEST_ONLY context must satisfy the same schema as deployed producer records.
  record_context.config_ref.present = true;
  record_context.config_ref.schema_version = 1U;
  record_context.config_ref.config_version = 1U;
  return record_context;
}

void assert_mq135_unavailable(
    const MqAdcReadResult& result,
    SampleQuality expected_quality = SampleQuality::Uncalibrated,
    SampleReason expected_reason = SampleReason::NotReady) {
  SequenceAllocator allocator;
  RecordFactory factory(allocator, context());
  MqFake source{};
  source.result = result;
  MqAdcProfile profile = profile_for(ProfileTarget::Mq135);
  MqAdcAcquirer acquirer(SensorId::Mq135, StreamId::Organic, source, factory, &profile);
  SampleRecord record{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(3000U, record)));
  TEST_ASSERT_EQUAL_INT(1, source.reads);
  TEST_ASSERT_TRUE(record.raw.present);
  TEST_ASSERT_EQUAL_UINT16(result.counts, record.raw.adc_counts);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(expected_quality),
                        static_cast<int>(record.quality));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(expected_reason),
                        static_cast<int>(record.reason));
  TEST_ASSERT_FALSE(record.converted.primary_available);
  TEST_ASSERT_FALSE(record.converted.secondary_available);
  TEST_ASSERT_FALSE(mq_adc_calibration_matches(record.raw.adc_calibration,
                                               record.raw.adc_counts));
  sigap::storage::ImmutableRecordBytes encoded{};
  TEST_ASSERT_TRUE(sigap::storage::RecordCodec::encode_sample(record, encoded));
  sigap::telemetry_archive::BatchJsonBuilder builder{};
  sigap::telemetry_archive::ImmutableBatch batch{};
  TEST_ASSERT_TRUE(builder.build(encoded, batch));
  const auto body = batch.exact_body();
  TEST_ASSERT_TRUE(body.find("\"adc_efuse_calibrated\":1") == std::string_view::npos);
}

}  // namespace

// A fresh sample carries no calibration claim at all.
void test_default_metadata_claims_nothing() {
  const MqAdcReadResult result{};
  TEST_ASSERT_FALSE(result.ok);
  TEST_ASSERT_FALSE(result.calibrated.available);
  TEST_ASSERT_FALSE(result.calibrated.same_sample);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcCalibrationMethod::None),
                        static_cast<int>(result.calibrated.method));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcCalibrationFailure::NotAttempted),
                        static_cast<int>(result.calibrated.failure));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcVoltageUnit::None),
                        static_cast<int>(result.calibrated.unit));
  TEST_ASSERT_EQUAL_UINT8(0U, result.calibrated.resolution_bits);
  TEST_ASSERT_EQUAL_UINT8(0U, result.calibrated.attenuation_db);
  TEST_ASSERT_EQUAL_UINT16(0U, result.calibrated.counts);
  TEST_ASSERT_EQUAL_FLOAT(0.0F, result.calibrated.pin_mv);

  const RawObservation raw{};
  TEST_ASSERT_FALSE(raw.adc_calibration.available);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcCalibrationFailure::NotAttempted),
                        static_cast<int>(raw.adc_calibration.failure));
}

// Unsupported eFuse must be explicitly unavailable, never 0 mV or 3300 mV.
void test_unsupported_efuse_is_explicitly_unavailable() {
  MqAdcCalibrationProbe probe{};
  probe.efuse_tp_fit_supported = false;
  const MqAdcCalibratedVoltage evidence = mq_adc_calibrated_voltage(probe, 2700U, 12U, 11U, 1U, 4U);
  TEST_ASSERT_FALSE(evidence.available);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcCalibrationFailure::EfuseUnsupported),
                        static_cast<int>(evidence.failure));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcCalibrationMethod::None),
                        static_cast<int>(evidence.method));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcVoltageUnit::None),
                        static_cast<int>(evidence.unit));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, evidence.pin_mv);
  TEST_ASSERT_FALSE(evidence.same_sample);
}

void test_rejected_characterization_or_conversion_is_unavailable() {
  MqAdcCalibrationProbe probe{};
  probe.efuse_tp_fit_supported = true;  // characterize returned something else
  MqAdcCalibratedVoltage evidence = mq_adc_calibrated_voltage(probe, 2700U, 12U, 11U, 1U, 4U);
  TEST_ASSERT_FALSE(evidence.available);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcCalibrationFailure::CharacterizeUnsupported),
                        static_cast<int>(evidence.failure));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, evidence.pin_mv);

  probe.characterized_tp_fit = true;  // characterization ok, conversion did not run
  evidence = mq_adc_calibrated_voltage(probe, 2700U, 12U, 11U, 1U, 4U);
  TEST_ASSERT_FALSE(evidence.available);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcCalibrationFailure::ConversionFailed),
                        static_cast<int>(evidence.failure));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, evidence.pin_mv);
}

void test_successful_conversion_reports_complete_metadata() {
  const MqAdcCalibratedVoltage evidence =
      mq_adc_calibrated_voltage(supported_probe(1600U), 2700U, 12U, 11U, 1U, 4U);
  TEST_ASSERT_TRUE(evidence.available);
  TEST_ASSERT_TRUE(evidence.same_sample);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcCalibrationMethod::EspAdcCalEfuseTpFit),
                        static_cast<int>(evidence.method));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcCalibrationFailure::None),
                        static_cast<int>(evidence.failure));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcVoltageUnit::Millivolt),
                        static_cast<int>(evidence.unit));
  TEST_ASSERT_EQUAL_UINT8(12U, evidence.resolution_bits);
  TEST_ASSERT_EQUAL_UINT8(11U, evidence.attenuation_db);
  TEST_ASSERT_EQUAL_UINT8(1U, evidence.adc_unit);
  TEST_ASSERT_EQUAL_UINT8(4U, evidence.adc_channel);
  TEST_ASSERT_EQUAL_UINT16(2700U, evidence.counts);
  TEST_ASSERT_EQUAL_FLOAT(1600.0F, evidence.pin_mv);
}

void test_raw_out_of_range_is_rejected_before_conversion() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcCalibrationFailure::RawOutOfRange),
                        static_cast<int>(
                            mq_adc_calibrated_voltage(supported_probe(1600U), 0U, 12U, 11U, 1U, 4U)
                                .failure));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcCalibrationFailure::RawOutOfRange),
                        static_cast<int>(
                            mq_adc_calibrated_voltage(supported_probe(1600U), 4095U, 12U, 11U, 1U, 4U)
                                .failure));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcCalibrationFailure::RawOutOfRange),
                        static_cast<int>(
                            mq_adc_calibrated_voltage(supported_probe(1600U), 4096U, 12U, 11U, 1U, 4U)
                                .failure));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcCalibrationFailure::RawOutOfRange),
                        static_cast<int>(
                            mq_adc_calibrated_voltage(supported_probe(1600U), 2700U, 0U, 11U, 1U, 4U)
                                .failure));
  const MqAdcCalibratedVoltage evidence =
      mq_adc_calibrated_voltage(supported_probe(1600U), 4096U, 12U, 11U, 1U, 4U);
  TEST_ASSERT_FALSE(evidence.available);
  TEST_ASSERT_EQUAL_FLOAT(0.0F, evidence.pin_mv);
}

void test_implausible_converted_voltage_is_rejected() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcCalibrationFailure::ImplausibleVoltage),
                        static_cast<int>(
                            mq_adc_calibrated_voltage(supported_probe(0U), 2700U, 12U, 11U, 1U, 4U)
                                .failure));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcCalibrationFailure::ImplausibleVoltage),
                        static_cast<int>(
                            mq_adc_calibrated_voltage(supported_probe(3301U), 2700U, 12U, 11U, 1U, 4U)
                                .failure));
}

void test_calibration_match_rejects_failure_or_wrong_effective_channel() {
  MqAdcCalibratedVoltage evidence =
      mq_adc_calibrated_voltage(supported_probe(1600U), 2700U, 12U, 11U, 1U, 4U);
  TEST_ASSERT_TRUE(mq_adc_calibration_matches(evidence, 2700U));
  evidence.failure = MqAdcCalibrationFailure::EfuseUnsupported;
  TEST_ASSERT_FALSE(mq_adc_calibration_matches(evidence, 2700U));

  evidence = mq_adc_calibrated_voltage(supported_probe(1600U), 2700U, 12U, 11U, 1U, 4U);
  evidence.attenuation_db = 10U;
  TEST_ASSERT_FALSE(mq_adc_calibration_matches(evidence, 2700U));
  evidence = mq_adc_calibrated_voltage(supported_probe(1600U), 2700U, 12U, 11U, 1U, 4U);
  evidence.adc_unit = 2U;
  TEST_ASSERT_FALSE(mq_adc_calibration_matches(evidence, 2700U));
  evidence = mq_adc_calibrated_voltage(supported_probe(1600U), 2700U, 12U, 11U, 1U, 4U);
  evidence.adc_channel = 3U;
  TEST_ASSERT_FALSE(mq_adc_calibration_matches(evidence, 2700U));
}

// Existing four-field aggregate initialisers must keep compiling and default the
// new metadata instead of requiring every call site to change.
void test_existing_aggregate_initialisers_are_preserved() {
  const MqAdcReadResult disconnected{false, true, 0U, SampleReason::Disconnected};
  TEST_ASSERT_TRUE(disconnected.disconnected);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleReason::Disconnected),
                        static_cast<int>(disconnected.reason));
  TEST_ASSERT_FALSE(disconnected.calibrated.available);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcCalibrationFailure::NotAttempted),
                        static_cast<int>(disconnected.calibrated.failure));
}

// MQ135 must derive AO from same-sample eFuse pin mV, not the legacy linear estimate.
void test_acquirer_uses_same_sample_calibration_for_mq135_voltage() {
  SequenceAllocator allocator;
  RecordFactory factory(allocator, context());
  MqFake source{};
  source.result.calibrated =
      mq_adc_calibrated_voltage(supported_probe(1600U), 2700U, 12U, 11U, 1U, 4U);
  MqAdcProfile profile = profile_for(ProfileTarget::Mq135);
  MqAdcAcquirer acquirer(SensorId::Mq135, StreamId::Organic, source, factory, &profile);
  SampleRecord record{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(1000U, record)));
  TEST_ASSERT_EQUAL_INT(1, source.reads);
  // Raw sample and source time are untouched.
  TEST_ASSERT_TRUE(record.raw.present);
  TEST_ASSERT_EQUAL_UINT16(2700U, record.raw.adc_counts);
  TEST_ASSERT_EQUAL_UINT64(1000U, record.header.monotonic_us);
  // Same-sample calibrated evidence is carried unchanged.
  TEST_ASSERT_TRUE(record.raw.adc_calibration.available);
  TEST_ASSERT_TRUE(record.raw.adc_calibration.same_sample);
  TEST_ASSERT_EQUAL_UINT16(2700U, record.raw.adc_calibration.counts);
  TEST_ASSERT_EQUAL_FLOAT(1600.0F, record.raw.adc_calibration.pin_mv);
  TEST_ASSERT_EQUAL_UINT8(12U, record.raw.adc_calibration.resolution_bits);
  TEST_ASSERT_EQUAL_UINT8(11U, record.raw.adc_calibration.attenuation_db);
  // The same-sample eFuse pin voltage wins; the current 5/3 divider is applied once.
  TEST_ASSERT_TRUE(record.converted.primary_available);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConvertedKind::AdcMillivolts),
                        static_cast<int>(record.converted.primary_kind));
  TEST_ASSERT_FLOAT_WITHIN(0.5F, 2666.67F, record.converted.primary_value);
  TEST_ASSERT_TRUE(record.converted.secondary_available);
  TEST_ASSERT_FLOAT_WITHIN(0.5F, 1600.0F, record.converted.secondary_value);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConvertedKind::AdcPinMillivolts),
                        static_cast<int>(record.converted.secondary_kind));
  TEST_ASSERT_FALSE(record.gas_ppm_available);

  // Exercise the real durable codec and telemetry batch writer end to end.
  sigap::storage::ImmutableRecordBytes encoded{};
  TEST_ASSERT_TRUE(sigap::storage::RecordCodec::encode_sample(record, encoded));
  SampleRecord decoded{};
  TEST_ASSERT_TRUE(sigap::storage::RecordCodec::decode_sample(encoded, decoded));
  TEST_ASSERT_TRUE(decoded.converted.secondary_available);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConvertedKind::AdcPinMillivolts),
                        static_cast<int>(decoded.converted.secondary_kind));
  TEST_ASSERT_FALSE(decoded.raw.adc_calibration.available);
  sigap::telemetry_archive::BatchJsonBuilder builder{};
  sigap::telemetry_archive::ImmutableBatch batch{};
  TEST_ASSERT_TRUE(builder.build(encoded, batch));
  const auto body = batch.exact_body();
  TEST_ASSERT_TRUE(body.find("\"adc_mv\":2666.67") != std::string_view::npos);
  TEST_ASSERT_TRUE(body.find("\"adc_pin_mv\":1600") != std::string_view::npos);
  TEST_ASSERT_TRUE(body.find("\"sensor_ao_mv\":2666.67") != std::string_view::npos);
  TEST_ASSERT_TRUE(body.find("\"adc_efuse_calibrated\":1") != std::string_view::npos);
  std::printf("TEST_ONLY_HTTP_BATCH_BEGIN");
  std::printf("%.*s", static_cast<int>(body.size()), body.data());
  std::printf("TEST_ONLY_HTTP_BATCH_END");

  SampleRecord legacy = record;
  legacy.converted.primary_value = 4500.0F;
  legacy.converted.secondary_available = false;
  legacy.converted.secondary_value = 0.0F;
  legacy.converted.secondary_kind = ConvertedKind::None;
  sigap::storage::ImmutableRecordBytes legacy_encoded{};
  TEST_ASSERT_TRUE(sigap::storage::RecordCodec::encode_sample(legacy, legacy_encoded));
  SampleRecord legacy_decoded{};
  TEST_ASSERT_TRUE(sigap::storage::RecordCodec::decode_sample(legacy_encoded,
                                                              legacy_decoded));
  TEST_ASSERT_FALSE(legacy_decoded.converted.secondary_available);
  sigap::telemetry_archive::ImmutableBatch legacy_batch{};
  TEST_ASSERT_TRUE(builder.build(legacy_encoded, legacy_batch));
  const auto legacy_body = legacy_batch.exact_body();
  TEST_ASSERT_TRUE(legacy_body.find("\"adc_mv\":4500") != std::string_view::npos);
  TEST_ASSERT_TRUE(legacy_body.find("adc_efuse_calibrated") == std::string_view::npos);
  TEST_ASSERT_TRUE(legacy_body.find("adc_pin_mv") == std::string_view::npos);
  TEST_ASSERT_TRUE(legacy_body.find("sensor_ao_mv") == std::string_view::npos);

  SampleRecord contradictory = record;
  contradictory.converted.secondary_value = 3301.0F;
  sigap::storage::ImmutableRecordBytes invalid_encoded{};
  TEST_ASSERT_TRUE(sigap::storage::RecordCodec::encode_sample(contradictory,
                                                              invalid_encoded));
  sigap::telemetry_archive::ImmutableBatch invalid_batch{};
  TEST_ASSERT_TRUE(builder.build(invalid_encoded, invalid_batch));
  const auto invalid_body = invalid_batch.exact_body();
  TEST_ASSERT_TRUE(invalid_body.find("adc_mv") == std::string_view::npos);
  TEST_ASSERT_TRUE(invalid_body.find("adc_efuse_calibrated") == std::string_view::npos);
}

void test_mq135_has_no_voltage_without_valid_same_sample_calibration() {
  const MqAdcReadResult missing{true, false, 2700U, SampleReason::DataReady};
  assert_mq135_unavailable(missing);

  MqAdcReadResult mismatch{true, false, 2700U, SampleReason::DataReady};
  mismatch.calibrated =
      mq_adc_calibrated_voltage(supported_probe(1600U), 2699U, 12U, 11U, 1U, 4U);
  assert_mq135_unavailable(mismatch);

  MqAdcCalibrationProbe unsupported{};
  MqAdcReadResult no_efuse{true, false, 2700U, SampleReason::DataReady};
  no_efuse.calibrated =
      mq_adc_calibrated_voltage(unsupported, 2700U, 12U, 11U, 1U, 4U);
  assert_mq135_unavailable(no_efuse);

  MqAdcReadResult saturated{true, false, 4095U, SampleReason::DataReady};
  saturated.calibrated =
      mq_adc_calibrated_voltage(supported_probe(3300U), 4095U, 12U, 11U, 1U, 4U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcCalibrationFailure::RawOutOfRange),
                        static_cast<int>(saturated.calibrated.failure));
  assert_mq135_unavailable(saturated, SampleQuality::OutOfRange,
                           SampleReason::OutOfRange);
}

void test_other_sensors_keep_no_calibration_evidence() {
  SequenceAllocator allocator;
  RecordFactory factory(allocator, context());
  MqFake source{};
  MqAdcProfile profile = profile_for(ProfileTarget::Mq4);
  MqAdcAcquirer acquirer(SensorId::Mq4, StreamId::Organic, source, factory, &profile);
  SampleRecord record{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(2000U, record)));
  TEST_ASSERT_FALSE(record.raw.adc_calibration.available);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MqAdcCalibrationFailure::NotAttempted),
                        static_cast<int>(record.raw.adc_calibration.failure));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, record.raw.adc_calibration.pin_mv);
  TEST_ASSERT_TRUE(record.converted.primary_available);
  TEST_ASSERT_EQUAL_FLOAT(4500.0F, record.converted.primary_value);
  TEST_ASSERT_EQUAL_INT(1, source.reads);
  sigap::storage::ImmutableRecordBytes encoded{};
  TEST_ASSERT_TRUE(sigap::storage::RecordCodec::encode_sample(record, encoded));
  sigap::telemetry_archive::BatchJsonBuilder builder{};
  sigap::telemetry_archive::ImmutableBatch batch{};
  TEST_ASSERT_TRUE(builder.build(encoded, batch));
  const auto body = batch.exact_body();
  TEST_ASSERT_TRUE(body.find("\"adc_mv\":4500") != std::string_view::npos);
  TEST_ASSERT_TRUE(body.find("adc_efuse_calibrated") == std::string_view::npos);
  TEST_ASSERT_TRUE(body.find("adc_pin_mv") == std::string_view::npos);
  TEST_ASSERT_TRUE(body.find("sensor_ao_mv") == std::string_view::npos);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_default_metadata_claims_nothing);
  RUN_TEST(test_unsupported_efuse_is_explicitly_unavailable);
  RUN_TEST(test_rejected_characterization_or_conversion_is_unavailable);
  RUN_TEST(test_successful_conversion_reports_complete_metadata);
  RUN_TEST(test_raw_out_of_range_is_rejected_before_conversion);
  RUN_TEST(test_implausible_converted_voltage_is_rejected);
  RUN_TEST(test_calibration_match_rejects_failure_or_wrong_effective_channel);
  RUN_TEST(test_existing_aggregate_initialisers_are_preserved);
  RUN_TEST(test_acquirer_uses_same_sample_calibration_for_mq135_voltage);
  RUN_TEST(test_mq135_has_no_voltage_without_valid_same_sample_calibration);
  RUN_TEST(test_other_sensors_keep_no_calibration_evidence);
  return UNITY_END();
}
