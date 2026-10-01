#include "nvs_config_store.h"

#ifdef ARDUINO

#include <array>
#include <algorithm>

#include <nvs.h>

#endif

namespace sigap::config {

#ifdef ARDUINO

namespace {
constexpr const char* kMetadataNamespace{"sigap_cfg_meta"};
constexpr const char* kActivationKey{"activation"};
constexpr const char* kProvisionedKey{"provisioned"};
constexpr const char* kBlobKey{"blob"};
constexpr uint32_t kActivationMagic{0x53414d31U};
constexpr uint8_t kActivationFormatVersion{1U};

void put_u64(uint8_t* bytes, std::size_t offset, uint64_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

uint64_t get_u64(const uint8_t* bytes, std::size_t offset) noexcept {
  uint64_t value{0};
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8U);
  }
  return value;
}

bool encode_activation(const ActivationMetadata& metadata,
                       std::array<uint8_t, kActivationMetadataBytes>& bytes) noexcept {
  bytes.fill(0U);
  bytes[0] = static_cast<uint8_t>(kActivationMagic);
  bytes[1] = static_cast<uint8_t>(kActivationMagic >> 8U);
  bytes[2] = static_cast<uint8_t>(kActivationMagic >> 16U);
  bytes[3] = static_cast<uint8_t>(kActivationMagic >> 24U);
  bytes[4] = kActivationFormatVersion;
  bytes[5] = static_cast<uint8_t>(metadata.active_slot);
  bytes[6] = metadata.has_previous ? 1U : 0U;
  bytes[7] = static_cast<uint8_t>(metadata.previous_slot);
  put_u64(bytes.data(), 8U, metadata.active_version);
  put_u64(bytes.data(), 16U, metadata.previous_version);
  std::copy(metadata.active_hash.begin(), metadata.active_hash.end(),
            bytes.begin() + 24U);
  std::copy(metadata.previous_hash.begin(), metadata.previous_hash.end(),
            bytes.begin() + 56U);
  return true;
}

bool decode_activation(const uint8_t* bytes,
                       ActivationMetadata& metadata) noexcept {
  const uint32_t magic = static_cast<uint32_t>(bytes[0]) |
                         (static_cast<uint32_t>(bytes[1]) << 8U) |
                         (static_cast<uint32_t>(bytes[2]) << 16U) |
                         (static_cast<uint32_t>(bytes[3]) << 24U);
  if (magic != kActivationMagic || bytes[4] != kActivationFormatVersion ||
      bytes[5] > 1U || bytes[6] > 1U || bytes[7] > 1U) {
    return false;
  }
  metadata.active_slot = static_cast<ConfigSlot>(bytes[5]);
  metadata.has_previous = bytes[6] != 0U;
  metadata.previous_slot = static_cast<ConfigSlot>(bytes[7]);
  metadata.active_version = get_u64(bytes, 8U);
  metadata.previous_version = get_u64(bytes, 16U);
  std::copy_n(bytes + 24U, kConfigHashBytes, metadata.active_hash.begin());
  std::copy_n(bytes + 56U, kConfigHashBytes, metadata.previous_hash.begin());
  return !metadata.has_previous ||
         metadata.previous_version < metadata.active_version;
}

StoreReadStatus open_readonly(const char* name, nvs_handle_t& handle) noexcept {
  const esp_err_t result = nvs_open(name, NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return StoreReadStatus::NotFound;
  return result == ESP_OK ? StoreReadStatus::Found : StoreReadStatus::Error;
}

bool open_readwrite(const char* name, nvs_handle_t& handle) noexcept {
  return nvs_open(name, NVS_READWRITE, &handle) == ESP_OK;
}
}

const char* NvsConfigStore::slot_namespace(ConfigSlot slot) noexcept {
  return slot == ConfigSlot::A ? "sigap_cfg_a" : "sigap_cfg_b";
}

StoreReadStatus NvsConfigStore::read_activation_metadata(
    ActivationMetadata& metadata) const noexcept {
  nvs_handle_t handle{};
  const StoreReadStatus open_status = open_readonly(kMetadataNamespace, handle);
  if (open_status != StoreReadStatus::Found) return open_status;

  std::size_t size{0};
  esp_err_t result = nvs_get_blob(handle, kActivationKey, nullptr, &size);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return StoreReadStatus::NotFound;
  }
  if (result != ESP_OK) {
    nvs_close(handle);
    return StoreReadStatus::Error;
  }
  if (size != kActivationMetadataBytes) {
    nvs_close(handle);
    return StoreReadStatus::Invalid;
  }

  std::array<uint8_t, kActivationMetadataBytes> bytes{};
  result = nvs_get_blob(handle, kActivationKey, bytes.data(), &size);
  nvs_close(handle);
  if (result != ESP_OK || size != kActivationMetadataBytes) {
    return StoreReadStatus::Error;
  }
  return decode_activation(bytes.data(), metadata) ? StoreReadStatus::Found
                                                   : StoreReadStatus::Invalid;
}

StoreReadStatus NvsConfigStore::read_slot(ConfigSlot slot,
                                          EncodedConfig& encoded) const noexcept {
  nvs_handle_t handle{};
  const StoreReadStatus open_status = open_readonly(slot_namespace(slot), handle);
  if (open_status != StoreReadStatus::Found) return open_status;

  std::size_t size{0};
  esp_err_t result = nvs_get_blob(handle, kBlobKey, nullptr, &size);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return StoreReadStatus::NotFound;
  }
  if (result != ESP_OK) {
    nvs_close(handle);
    return StoreReadStatus::Error;
  }
  if (size != kEncodedConfigBytes) {
    nvs_close(handle);
    return StoreReadStatus::Invalid;
  }
  result = nvs_get_blob(handle, kBlobKey, encoded.bytes.data(), &size);
  nvs_close(handle);
  if (result != ESP_OK || size != kEncodedConfigBytes) {
    return StoreReadStatus::Error;
  }
  encoded.size = size;
  return StoreReadStatus::Found;
}

bool NvsConfigStore::write_slot(ConfigSlot slot,
                                const EncodedConfig& encoded) noexcept {
  if (encoded.size != kEncodedConfigBytes) {
    return false;
  }
  nvs_handle_t handle{};
  if (!open_readwrite(slot_namespace(slot), handle)) return false;
  const esp_err_t set_result =
      nvs_set_blob(handle, kBlobKey, encoded.bytes.data(), encoded.size);
  const esp_err_t commit_result =
      set_result == ESP_OK ? nvs_commit(handle) : set_result;
  nvs_close(handle);
  return commit_result == ESP_OK;
}

bool NvsConfigStore::write_activation_metadata(
    const ActivationMetadata& metadata) noexcept {
  std::array<uint8_t, kActivationMetadataBytes> bytes{};
  if (!encode_activation(metadata, bytes)) return false;
  nvs_handle_t handle{};
  if (!open_readwrite(kMetadataNamespace, handle)) return false;
  const esp_err_t set_result =
      nvs_set_blob(handle, kActivationKey, bytes.data(), bytes.size());
  const esp_err_t commit_result =
      set_result == ESP_OK ? nvs_commit(handle) : set_result;
  nvs_close(handle);
  return commit_result == ESP_OK;
}

ProvisioningStatus NvsConfigStore::provisioning_status() const noexcept {
  nvs_handle_t handle{};
  const StoreReadStatus open_status = open_readonly(kMetadataNamespace, handle);
  if (open_status == StoreReadStatus::NotFound) {
    return ProvisioningStatus::Unprovisioned;
  }
  if (open_status == StoreReadStatus::Error) return ProvisioningStatus::Error;
  uint8_t value{0};
  const esp_err_t result = nvs_get_u8(handle, kProvisionedKey, &value);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return ProvisioningStatus::Unprovisioned;
  if (result != ESP_OK || value > 1U) return ProvisioningStatus::Error;
  return value == 0U ? ProvisioningStatus::Unprovisioned
                     : ProvisioningStatus::Provisioned;
}

bool NvsConfigStore::mark_provisioned() noexcept {
  nvs_handle_t handle{};
  if (!open_readwrite(kMetadataNamespace, handle)) return false;
  const esp_err_t set_result = nvs_set_u8(handle, kProvisionedKey, 1U);
  const esp_err_t commit_result =
      set_result == ESP_OK ? nvs_commit(handle) : set_result;
  nvs_close(handle);
  return commit_result == ESP_OK;
}

#else

StoreReadStatus NvsConfigStore::read_activation_metadata(
    ActivationMetadata&) const noexcept {
  return StoreReadStatus::Error;
}

StoreReadStatus NvsConfigStore::read_slot(ConfigSlot,
                                          EncodedConfig&) const noexcept {
  return StoreReadStatus::Error;
}

bool NvsConfigStore::write_slot(ConfigSlot,
                                const EncodedConfig&) noexcept {
  return false;
}

bool NvsConfigStore::write_activation_metadata(
    const ActivationMetadata&) noexcept {
  return false;
}

ProvisioningStatus NvsConfigStore::provisioning_status() const noexcept {
  return ProvisioningStatus::Error;
}

bool NvsConfigStore::mark_provisioned() noexcept { return false; }

#endif

}  // namespace sigap::config
