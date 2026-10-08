#include <string>

#include <unity.h>

#include "network/https_transport.h"

namespace {

using sigap::network::HttpPostRequest;
using sigap::network::HttpPostStatus;
using sigap::telemetry_archive::AckStatus;

void test_request_bounds_and_https_policy() {
  const HttpPostRequest valid{
      "https://example.invalid/api/telemetry/batches", "token", "{}", 1U};
  TEST_ASSERT_TRUE(sigap::network::valid_archive_http_request(valid));
  TEST_ASSERT_FALSE(sigap::network::valid_archive_http_request(
      HttpPostRequest{"http://example.invalid", "token", "{}", 1U}));
  TEST_ASSERT_FALSE(sigap::network::valid_archive_http_request(
      HttpPostRequest{"https://example.invalid", "", "{}", 1U}));
  TEST_ASSERT_FALSE(sigap::network::valid_archive_http_request(
      HttpPostRequest{"https://example.invalid", "token", "", 1U}));
  TEST_ASSERT_FALSE(sigap::network::valid_archive_http_request(
      HttpPostRequest{"https://example.invalid", "token", "{}", 0U}));
}

void test_status_and_retry_after_mapping() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpPostStatus::Delivered),
                        static_cast<int>(sigap::network::classify_http_status(200)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpPostStatus::Unauthorized),
                        static_cast<int>(sigap::network::classify_http_status(401)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpPostStatus::Conflict),
                        static_cast<int>(sigap::network::classify_http_status(409)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpPostStatus::Retryable),
                        static_cast<int>(sigap::network::classify_http_status(429)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpPostStatus::Retryable),
                        static_cast<int>(sigap::network::classify_http_status(503)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpPostStatus::ProtocolError),
                        static_cast<int>(sigap::network::classify_http_status(422)));
  TEST_ASSERT_EQUAL_UINT32(5000U,
                           sigap::network::parse_retry_after_ms(" 5 "));
  TEST_ASSERT_EQUAL_UINT32(0U,
                           sigap::network::parse_retry_after_ms("garbage"));
}

void test_durable_ack_parser_requires_all_identity_and_counts() {
  constexpr char kAck[] =
      "{\"device_id\":\"000102030405060708090a0b0c0d0e0f\","
      "\"batch_id\":\"device/boot/batch-1\","
      "\"body_hash\":\"sha256:000102030405060708090a0b0c0d0e0f"
      "101112131415161718191a1b1c1d1e1f\",\"record_count\":2,"
      "\"stored_count\":1,\"deduplicated_count\":1,"
      "\"status\":\"DURABLE\"}";
  sigap::telemetry_archive::Ack output{};
  TEST_ASSERT_TRUE(sigap::network::decode_archive_ack(kAck, output));
  TEST_ASSERT_TRUE(output.device_id.present);
  TEST_ASSERT_EQUAL_UINT16(2U, output.record_count);
  TEST_ASSERT_EQUAL_UINT16(1U, output.stored_count);
  TEST_ASSERT_EQUAL_UINT16(1U, output.deduplicated_count);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckStatus::Durable),
                        static_cast<int>(output.status));
  TEST_ASSERT_EQUAL_STRING("device/boot/batch-1",
                          std::string(output.batch_id.data(), output.batch_id_size).c_str());
}

void test_durable_ack_parser_rejects_mismatch_and_duplicate_fields() {
  constexpr char kMissing[] =
      "{\"device_id\":\"000102030405060708090a0b0c0d0e0f\","
      "\"batch_id\":\"b\",\"status\":\"DURABLE\"}";
  constexpr char kDuplicate[] =
      "{\"device_id\":\"000102030405060708090a0b0c0d0e0f\","
      "\"device_id\":\"000102030405060708090a0b0c0d0e0f\","
      "\"batch_id\":\"b\",\"body_hash\":\"sha256:"
      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\","
      "\"record_count\":1,\"stored_count\":1,"
      "\"deduplicated_count\":0,\"status\":\"DURABLE\"}";
  sigap::telemetry_archive::Ack output{};
  TEST_ASSERT_FALSE(sigap::network::decode_archive_ack(kMissing, output));
  TEST_ASSERT_FALSE(sigap::network::decode_archive_ack(kDuplicate, output));
}

void test_durable_ack_parser_rejects_unknown_nested_trailing_and_wrong_types() {
  constexpr char kUnknown[] =
      "{\"device_id\":\"000102030405060708090a0b0c0d0e0f\","
      "\"batch_id\":\"b\",\"body_hash\":\"sha256:"
      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\","
      "\"record_count\":1,\"stored_count\":1,\"deduplicated_count\":0,"
      "\"status\":\"DURABLE\",\"extra\":1}";
  constexpr char kNested[] =
      "{\"device_id\":{\"value\":\"000102030405060708090a0b0c0d0e0f\"},"
      "\"batch_id\":\"b\",\"body_hash\":\"sha256:"
      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\","
      "\"record_count\":1,\"stored_count\":1,\"deduplicated_count\":0,"
      "\"status\":\"DURABLE\"}";
  constexpr char kTrailing[] = "{\"status\":\"DURABLE\"} trailing";
  constexpr char kWrongType[] = "{\"status\":1}";
  sigap::telemetry_archive::Ack output{};
  TEST_ASSERT_FALSE(sigap::network::decode_archive_ack(kUnknown, output));
  TEST_ASSERT_FALSE(sigap::network::decode_archive_ack(kNested, output));
  TEST_ASSERT_FALSE(sigap::network::decode_archive_ack(kTrailing, output));
  TEST_ASSERT_FALSE(sigap::network::decode_archive_ack(kWrongType, output));
}

void test_durable_ack_parser_rejects_escaped_and_duplicate_semantic_keys() {
  constexpr char kEscaped[] =
      "{\"device_id\":\"000102030405060708090a0b0c0d0e0f\","
      "\"batch_id\":\"b\\u002f1\",\"body_hash\":\"sha256:"
      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\","
      "\"record_count\":1,\"stored_count\":1,\"deduplicated_count\":0,"
      "\"status\":\"DURABLE\"}";
  constexpr char kLeadingZero[] =
      "{\"device_id\":\"000102030405060708090a0b0c0d0e0f\","
      "\"batch_id\":\"b\",\"body_hash\":\"sha256:"
      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\","
      "\"record_count\":01,\"stored_count\":1,\"deduplicated_count\":0,"
      "\"status\":\"DURABLE\"}";
  sigap::telemetry_archive::Ack output{};
  TEST_ASSERT_FALSE(sigap::network::decode_archive_ack(kEscaped, output));
  TEST_ASSERT_FALSE(sigap::network::decode_archive_ack(kLeadingZero, output));
}

void test_network_bounds_remain_frozen() {
  TEST_ASSERT_EQUAL_UINT32(512U,
                           static_cast<uint32_t>(sigap::network::kMaxResponseBodyBytes));
  TEST_ASSERT_EQUAL_UINT32(32768U,
                           static_cast<uint32_t>(sigap::network::kMaxArchiveBodyBytes));
  TEST_ASSERT_EQUAL_UINT32(128U,
                           static_cast<uint32_t>(sigap::network::kMaxArchiveRecords));
}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_request_bounds_and_https_policy);
  RUN_TEST(test_status_and_retry_after_mapping);
  RUN_TEST(test_durable_ack_parser_requires_all_identity_and_counts);
  RUN_TEST(test_durable_ack_parser_rejects_mismatch_and_duplicate_fields);
  RUN_TEST(test_durable_ack_parser_rejects_unknown_nested_trailing_and_wrong_types);
  RUN_TEST(test_durable_ack_parser_rejects_escaped_and_duplicate_semantic_keys);
  RUN_TEST(test_network_bounds_remain_frozen);
  UNITY_END();
}

void loop() {}

#ifndef ARDUINO
int main() {
  setup();
  return 0;
}
#endif
