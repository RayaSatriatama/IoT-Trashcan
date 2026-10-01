#include <cstring>

#include <unity.h>

#include "provisioning/secret_store.h"

namespace {

using sigap::provisioning::MemorySecretBlobStore;
using sigap::provisioning::NetworkSecrets;
using sigap::provisioning::PersistentSecretStore;
using sigap::provisioning::SecretBlobCodec;
using sigap::provisioning::SecretSlot;
using sigap::provisioning::SecretStoreResult;

void test_initial_status_is_absent() {
  MemorySecretBlobStore backend;
  PersistentSecretStore store(backend);
  const auto presence = store.status();
  TEST_ASSERT_FALSE(presence.wifi_configured);
  TEST_ASSERT_FALSE(presence.device_token_configured);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SecretStoreResult::NotFound),
                        static_cast<int>(store.last_result()));
}

void test_wifi_pair_is_persisted_as_one_valid_blob() {
  MemorySecretBlobStore backend;
  PersistentSecretStore store(backend);
  TEST_ASSERT_TRUE(store.set_wifi("SIGAP-WIFI", "WIFI_SENTINEL_9137"));
  TEST_ASSERT_TRUE(backend.present(SecretSlot::Wifi));

  NetworkSecrets output{};
  TEST_ASSERT_FALSE(store.load_for_network(output));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SecretStoreResult::NotFound),
                        static_cast<int>(store.last_result()));
  TEST_ASSERT_TRUE(store.rotate_device_token("TOKEN_SENTINEL_9137"));
  TEST_ASSERT_TRUE(store.load_for_network(output));
  TEST_ASSERT_EQUAL_STRING("SIGAP-WIFI", output.ssid.data());
  TEST_ASSERT_EQUAL_STRING("WIFI_SENTINEL_9137", output.password.data());
  TEST_ASSERT_EQUAL_STRING("TOKEN_SENTINEL_9137", output.device_token.data());
  sigap::provisioning::secure_zero(&output, sizeof(output));
}

void test_token_rotation_replaces_old_blob() {
  MemorySecretBlobStore backend;
  PersistentSecretStore store(backend);
  TEST_ASSERT_TRUE(store.rotate_device_token("TOKEN_OLD_9137"));
  TEST_ASSERT_TRUE(store.rotate_device_token("TOKEN_NEW_9137"));
  NetworkSecrets output{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SecretStoreResult::Ok),
                        static_cast<int>(store.last_result()));
  TEST_ASSERT_FALSE(store.load_for_network(output));
  TEST_ASSERT_TRUE(store.set_wifi("SSID", "PASSWORD"));
  TEST_ASSERT_TRUE(store.load_for_network(output));
  TEST_ASSERT_EQUAL_STRING("TOKEN_NEW_9137", output.device_token.data());
  TEST_ASSERT_NOT_EQUAL(0, std::strcmp(output.device_token.data(), "TOKEN_OLD_9137"));
  sigap::provisioning::secure_zero(&output, sizeof(output));
}

void test_codec_round_trip_is_bounded_and_deterministic() {
  sigap::provisioning::EncodedSecretBlob first{};
  sigap::provisioning::EncodedSecretBlob second{};
  TEST_ASSERT_TRUE(SecretBlobCodec::encode_wifi("SSID", "PASSWORD", first));
  TEST_ASSERT_TRUE(SecretBlobCodec::encode_wifi("SSID", "PASSWORD", second));
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(first.size),
                           static_cast<uint32_t>(second.size));
  TEST_ASSERT_EQUAL_MEMORY(first.bytes.data(), second.bytes.data(), first.size);
  NetworkSecrets decoded{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SecretStoreResult::Ok),
                        static_cast<int>(SecretBlobCodec::decode_wifi(
                            first.bytes.data(), first.size, decoded)));
  TEST_ASSERT_EQUAL_STRING("SSID", decoded.ssid.data());
  TEST_ASSERT_EQUAL_STRING("PASSWORD", decoded.password.data());
  sigap::provisioning::secure_zero(&decoded, sizeof(decoded));
}

void test_corrupt_blob_is_not_configured() {
  MemorySecretBlobStore backend;
  PersistentSecretStore store(backend);
  TEST_ASSERT_TRUE(store.set_wifi("SSID", "PASSWORD"));
  TEST_ASSERT_TRUE(store.rotate_device_token("TOKEN"));
  backend.corrupt(SecretSlot::Wifi);
  const auto presence = store.status();
  TEST_ASSERT_FALSE(presence.wifi_configured);
  TEST_ASSERT_TRUE(presence.device_token_configured);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SecretStoreResult::Corrupt),
                        static_cast<int>(store.last_result()));
}

void test_unsupported_schema_is_rejected() {
  sigap::provisioning::EncodedSecretBlob encoded{};
  TEST_ASSERT_TRUE(SecretBlobCodec::encode_token("TOKEN", encoded));
  encoded.bytes[4] = 99U;
  NetworkSecrets output{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SecretStoreResult::UnsupportedSchema),
                        static_cast<int>(SecretBlobCodec::decode_token(
                            encoded.bytes.data(), encoded.size, output)));
  sigap::provisioning::secure_zero(&output, sizeof(output));
}

void test_truncated_blob_is_rejected() {
  sigap::provisioning::EncodedSecretBlob encoded{};
  TEST_ASSERT_TRUE(SecretBlobCodec::encode_token("TOKEN", encoded));
  NetworkSecrets output{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SecretStoreResult::Corrupt),
                        static_cast<int>(SecretBlobCodec::decode_token(
                            encoded.bytes.data(), encoded.size - 1U, output)));
  sigap::provisioning::secure_zero(&output, sizeof(output));
}

void test_failed_write_preserves_previous_secret() {
  MemorySecretBlobStore backend;
  PersistentSecretStore store(backend);
  TEST_ASSERT_TRUE(store.set_wifi("SSID", "PASSWORD_OLD"));
  backend.fail_next(MemorySecretBlobStore::Fault::WriteWifi);
  TEST_ASSERT_FALSE(store.set_wifi("SSID", "PASSWORD_NEW"));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SecretStoreResult::StorageError),
                        static_cast<int>(store.last_result()));
  TEST_ASSERT_TRUE(store.rotate_device_token("TOKEN"));
  NetworkSecrets output{};
  TEST_ASSERT_TRUE(store.load_for_network(output));
  TEST_ASSERT_EQUAL_STRING("PASSWORD_OLD", output.password.data());
  sigap::provisioning::secure_zero(&output, sizeof(output));
}

void test_clear_requires_confirmation_and_erases_both_blobs() {
  MemorySecretBlobStore backend;
  PersistentSecretStore store(backend);
  TEST_ASSERT_TRUE(store.set_wifi("SSID", "PASSWORD"));
  TEST_ASSERT_TRUE(store.rotate_device_token("TOKEN"));
  TEST_ASSERT_FALSE(store.clear_all(false));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SecretStoreResult::ConfirmationRequired),
                        static_cast<int>(store.last_result()));
  TEST_ASSERT_TRUE(backend.present(SecretSlot::Wifi));
  TEST_ASSERT_TRUE(store.clear_all(true));
  const auto presence = store.status();
  TEST_ASSERT_FALSE(presence.wifi_configured);
  TEST_ASSERT_FALSE(presence.device_token_configured);
  TEST_ASSERT_FALSE(backend.present(SecretSlot::Wifi));
  TEST_ASSERT_FALSE(backend.present(SecretSlot::DeviceToken));
}

void test_clear_failure_preserves_both_blobs() {
  MemorySecretBlobStore backend;
  PersistentSecretStore store(backend);
  TEST_ASSERT_TRUE(store.set_wifi("SSID", "PASSWORD"));
  TEST_ASSERT_TRUE(store.rotate_device_token("TOKEN"));
  backend.fail_next(MemorySecretBlobStore::Fault::Clear);
  TEST_ASSERT_FALSE(store.clear_all(true));
  TEST_ASSERT_TRUE(backend.present(SecretSlot::Wifi));
  TEST_ASSERT_TRUE(backend.present(SecretSlot::DeviceToken));
}

void test_invalid_secret_input_is_rejected_without_persistence() {
  MemorySecretBlobStore backend;
  PersistentSecretStore store(backend);
  TEST_ASSERT_FALSE(store.set_wifi(nullptr, "PASSWORD"));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SecretStoreResult::InvalidInput),
                        static_cast<int>(store.last_result()));
  TEST_ASSERT_FALSE(backend.present(SecretSlot::Wifi));
  TEST_ASSERT_FALSE(store.rotate_device_token(""));
  TEST_ASSERT_FALSE(backend.present(SecretSlot::DeviceToken));
}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_initial_status_is_absent);
  RUN_TEST(test_wifi_pair_is_persisted_as_one_valid_blob);
  RUN_TEST(test_token_rotation_replaces_old_blob);
  RUN_TEST(test_codec_round_trip_is_bounded_and_deterministic);
  RUN_TEST(test_corrupt_blob_is_not_configured);
  RUN_TEST(test_unsupported_schema_is_rejected);
  RUN_TEST(test_truncated_blob_is_rejected);
  RUN_TEST(test_failed_write_preserves_previous_secret);
  RUN_TEST(test_clear_requires_confirmation_and_erases_both_blobs);
  RUN_TEST(test_clear_failure_preserves_both_blobs);
  RUN_TEST(test_invalid_secret_input_is_rejected_without_persistence);
  UNITY_END();
}

void loop() {}

#ifndef ARDUINO
int main() {
  setup();
  return 0;
}
#endif
