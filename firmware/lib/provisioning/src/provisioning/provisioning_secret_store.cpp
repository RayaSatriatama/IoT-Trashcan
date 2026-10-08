#include "provisioning_secret_store.h"

#include <algorithm>

#ifdef ARDUINO
#include <esp_system.h>
#include <nvs.h>
#endif

namespace sigap::provisioning {

bool MemoryProvisioningSecretStore::load_or_create(
    std::array<uint8_t, kProvisioningSecretBytes>& output) noexcept {
  if (fail_) {
    fail_ = false;
    output.fill(0U);
    return false;
  }
  if (!present_) {
    for (auto& byte : value_) byte = static_cast<uint8_t>(0xA5U ^ byte);
    // A deterministic native value is sufficient for protocol tests.  Target
    // storage uses esp_random below and is never used as a production token.
    value_[0] = 0x53U;
    value_[1] = 0x49U;
    value_[2] = 0x47U;
    value_[3] = 0x41U;
    present_ = true;
  }
  output = value_;
  return true;
}

#ifdef ARDUINO
namespace {
constexpr const char* kNamespace{"sigap_ble"};
constexpr const char* kKey{"setup_secret"};
}

bool NvsProvisioningSecretStore::load_or_create(
    std::array<uint8_t, kProvisioningSecretBytes>& output) noexcept {
  nvs_handle_t handle{};
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    output.fill(0U);
    return false;
  }
  std::size_t size{kProvisioningSecretBytes};
  esp_err_t result = nvs_get_blob(handle, kKey, output.data(), &size);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    for (auto& byte : output) {
      uint32_t random_value{0U};
      esp_fill_random(&random_value, sizeof(random_value));
      byte = static_cast<uint8_t>(random_value);
    }
    result = nvs_set_blob(handle, kKey, output.data(), output.size());
    if (result == ESP_OK) result = nvs_commit(handle);
  } else if (result == ESP_OK && size != output.size()) {
    result = ESP_ERR_INVALID_SIZE;
  }
  nvs_close(handle);
  if (result != ESP_OK) output.fill(0U);
  return result == ESP_OK;
}
#else
bool NvsProvisioningSecretStore::load_or_create(
    std::array<uint8_t, kProvisioningSecretBytes>& output) noexcept {
  output.fill(0U);
  return false;
}
#endif

}  // namespace sigap::provisioning
