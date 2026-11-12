#pragma once
#include "config/nvs_config_store.h"
#include "config/config_codec.h"
#include "provisioning/secret_store.h"
#include "test_platform.h"
#include "nvs.h"

// Synthetic persistence, never physical NVS or production provisioning.
inline void seed_test_boot(bool with_token = true) {
  using namespace sigap;
  test_platform::nvs.clear();
  config::AppConfig value{};
  value.config_version = 1U;
  config::EncodedConfig encoded{};
  assert(config::ConfigCodec{}.encode(value, encoded));
  config::NvsConfigStore store;
  assert(store.write_slot(config::ConfigSlot::A, encoded));
  config::ActivationMetadata activation{};
  activation.active_slot = config::ConfigSlot::A;
  activation.active_version = value.config_version;
  activation.active_hash = encoded.hash();
  assert(store.write_activation_metadata(activation));
  assert(store.mark_provisioned());
  provisioning::NvsSecretStore secrets;
  assert(secrets.set_wifi("TEST-NETWORK", "TEST-PASSWORD"));
  if (with_token) assert(secrets.rotate_device_token("TEST-DEVICE-TOKEN"));
}
