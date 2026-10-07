#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <string>

#include <unity.h>

#include "maintenance/maintenance.h"
#include "maintenance/model_runtime.h"
#include "models/model_lifecycle.h"

namespace {

using namespace sigap;

models::SafetyModelProfile safety_fixture(uint32_t version) {
  models::SafetyModelProfile profile{};
  profile.manifest.kind = acquisition::ProfileKind::Safety;
  profile.manifest.schema_version = models::kSafetyProfileSchemaVersion;
  profile.manifest.profile_version = version;
  profile.manifest.target_id = models::kSafetyB3TargetId;
  profile.manifest.validity = acquisition::ProfileValidity::TestOnly;
  profile.deployment = models::ModelDeployment::TestOnly;
  profile.channel_count = 1U;
  profile.channels[0] = {models::HazardSignal::TemperatureC, true, 80.0F,
                         70.0F};
  profile.clear_consecutive_cycles = 2U;
  return profile;
}

maintenance::MaintenanceCommand command(maintenance::CommandKind kind,
                                         const char* target) {
  maintenance::MaintenanceCommand value{};
  value.kind = kind;
  TEST_ASSERT_TRUE(value.target.assign(target));
  return value;
}

std::string hex(const models::EncodedModelProfile& encoded) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string output;
  output.reserve(encoded.size * 2U);
  for (std::size_t index = 0U; index < encoded.size; ++index) {
    output.push_back(digits[encoded.bytes[index] >> 4U]);
    output.push_back(digits[encoded.bytes[index] & 0x0fU]);
  }
  return output;
}

std::string hash_hex(const models::EncodedModelProfile& encoded) {
  models::EncodedModelProfile value = encoded;
  return hex(value).substr((value.size - config::kConfigHashBytes) * 2U);
}

void run_line(maintenance::ModelMaintenanceRouter& router,
              maintenance::MaintenanceParser& parser,
              const std::string& line, uint64_t now_us) {
  maintenance::ParseEvent event{};
  bool emitted{false};
  for (const char character : line) {
    emitted = parser.consume(character, event) || emitted;
  }
  TEST_ASSERT_TRUE(emitted);
  maintenance::MaintenanceResponse response{};
  const auto result = router.execute_model(event.command, now_us, response);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(maintenance::CommandStatus::Accepted),
                        static_cast<int>(result));
}

void test_model_import_stages_then_requires_explicit_activation() {
  models::MemoryModelSlotStore store{};
  models::ModelProfileCodec codec{};
  models::ModelProfilePersistence persistence(store, codec);
  models::ModelProfileLifecycle lifecycle(persistence, codec);
  const models::ModelProfileKey key{acquisition::ProfileKind::Safety,
                                    models::ModelTarget::SafetyB3};
  TEST_ASSERT_TRUE(lifecycle.load(key));

  models::EncodedModelProfile encoded{};
  TEST_ASSERT_TRUE(codec.encode(safety_fixture(1U), encoded));
  TEST_ASSERT_TRUE(lifecycle.stage(key, encoded));
  TEST_ASSERT_TRUE(lifecycle.validate(key));
  TEST_ASSERT_TRUE(lifecycle.preview(key));
  TEST_ASSERT_TRUE(lifecycle.commit(key));

  maintenance::ModelMaintenanceRouter router(lifecycle, codec);
  auto status = command(maintenance::CommandKind::ModelStatus, "safety_b3");
  maintenance::MaintenanceResponse response{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(maintenance::CommandStatus::Accepted),
      static_cast<int>(router.execute_model(status, 1U, response)));
  TEST_ASSERT_NOT_NULL(response.view().data());

  models::ModelProfileSnapshot before{};
  TEST_ASSERT_TRUE(lifecycle.snapshot(key, before));
  TEST_ASSERT_TRUE(before.present);
  TEST_ASSERT_FALSE(lifecycle.stage(key, encoded));
  models::ModelProfileSnapshot after{};
  TEST_ASSERT_TRUE(lifecycle.snapshot(key, after));
  TEST_ASSERT_EQUAL_UINT32(before.ref.profile_version,
                           after.ref.profile_version);
}

void test_model_parser_binds_typed_commands() {
  maintenance::MaintenanceParser parser{};
  maintenance::ParseEvent event{};
  const char* line = "model import chunk 0 aa55\n";
  bool emitted = false;
  for (const char* cursor = line; *cursor != '\0'; ++cursor) {
    emitted = parser.consume(*cursor, event) || emitted;
  }
  TEST_ASSERT_TRUE(emitted);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(maintenance::CommandStatus::Accepted),
      static_cast<int>(event.status));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(maintenance::CommandKind::ModelImportChunk),
      static_cast<int>(event.command.kind));
  TEST_ASSERT_TRUE(event.command.has_numeric);
}

void test_model_transfer_finish_only_stages_until_explicit_commit() {
  models::MemoryModelSlotStore store{};
  models::ModelProfileCodec codec{};
  models::ModelProfilePersistence persistence(store, codec);
  models::ModelProfileLifecycle lifecycle(persistence, codec);
  const models::ModelProfileKey key{acquisition::ProfileKind::Safety,
                                    models::ModelTarget::SafetyB3};
  TEST_ASSERT_TRUE(lifecycle.load(key));
  models::EncodedModelProfile v1{};
  TEST_ASSERT_TRUE(codec.encode(safety_fixture(1U), v1));
  TEST_ASSERT_TRUE(lifecycle.stage(key, v1));
  TEST_ASSERT_TRUE(lifecycle.validate(key));
  TEST_ASSERT_TRUE(lifecycle.preview(key));
  TEST_ASSERT_TRUE(lifecycle.commit(key));

  models::EncodedModelProfile v2{};
  TEST_ASSERT_TRUE(codec.encode(safety_fixture(2U), v2));
  maintenance::ModelMaintenanceRouter router(lifecycle, codec);
  maintenance::MaintenanceParser parser{};
  run_line(router, parser,
           "model import begin safety_b3 " + std::to_string(v2.size) +
               " " + hash_hex(v2) + "\n",
           10U);
  const std::string encoded = hex(v2);
  for (std::size_t offset = 0U; offset < v2.size; offset += 20U) {
    const std::size_t bytes = std::min<std::size_t>(20U, v2.size - offset);
    run_line(router, parser,
             "model import chunk " + std::to_string(offset) + " " +
                 encoded.substr(offset * 2U, bytes * 2U) + "\n",
             11U + offset);
  }
  run_line(router, parser, "model import finish safety_b3\n", 100U);
  models::ModelProfileSnapshot staged{};
  TEST_ASSERT_TRUE(lifecycle.snapshot(key, staged));
  TEST_ASSERT_EQUAL_UINT32(1U, staged.ref.profile_version);
  run_line(router, parser, "model validate safety_b3\n", 101U);
  run_line(router, parser, "model preview safety_b3\n", 102U);
  run_line(router, parser, "model commit safety_b3\n", 103U);
  models::ModelProfileSnapshot active{};
  TEST_ASSERT_TRUE(lifecycle.snapshot(key, active));
  TEST_ASSERT_EQUAL_UINT32(2U, active.ref.profile_version);
}

void test_model_export_is_complete_and_non_repeating() {
  models::MemoryModelSlotStore store{};
  models::ModelProfileCodec codec{};
  models::ModelProfilePersistence persistence(store, codec);
  models::ModelProfileLifecycle lifecycle(persistence, codec);
  const models::ModelProfileKey key{acquisition::ProfileKind::Safety,
                                    models::ModelTarget::SafetyB3};
  TEST_ASSERT_TRUE(lifecycle.load(key));
  models::EncodedModelProfile encoded{};
  TEST_ASSERT_TRUE(codec.encode(safety_fixture(1U), encoded));
  TEST_ASSERT_TRUE(lifecycle.stage(key, encoded));
  TEST_ASSERT_TRUE(lifecycle.validate(key));
  TEST_ASSERT_TRUE(lifecycle.preview(key));
  TEST_ASSERT_TRUE(lifecycle.commit(key));

  maintenance::ModelMaintenanceRouter router(lifecycle, codec);
  std::string wire_data;
  bool first{true};
  std::size_t now{1U};
  while (wire_data.size() < encoded.size * 2U) {
    maintenance::MaintenanceCommand export_command =
        command(maintenance::CommandKind::ModelExport, "safety_b3");
    maintenance::MaintenanceResponse response{};
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(maintenance::CommandStatus::Accepted),
        static_cast<int>(router.execute_model(export_command, now++, response)));
    const std::string wire(response.view());
    const std::size_t marker = wire.find("DATA ");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, marker);
    if (first) {
      TEST_ASSERT_TRUE(wire.find("MODEL BEGIN size=") == 0U);
      first = false;
    }
    const std::size_t start = marker + 5U;
    const std::size_t end = wire.find('\n', start);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, end);
    wire_data.append(wire.substr(start, end - start));
    TEST_ASSERT_TRUE(response.size() <= maintenance::kMaxResponseBytes);
  }
  TEST_ASSERT_EQUAL_STRING(hex(encoded).c_str(), wire_data.c_str());
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_model_import_stages_then_requires_explicit_activation);
  RUN_TEST(test_model_parser_binds_typed_commands);
  RUN_TEST(test_model_transfer_finish_only_stages_until_explicit_commit);
  RUN_TEST(test_model_export_is_complete_and_non_repeating);
  return UNITY_END();
}
