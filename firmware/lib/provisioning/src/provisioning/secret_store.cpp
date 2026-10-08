#include "secret_store.h"

#include <algorithm>
#include <string_view>

#include "telemetry/sha256.h"

#ifdef ARDUINO
#include <nvs.h>
#endif

namespace sigap::provisioning {
namespace {

constexpr uint32_t kSecretMagic{0x53475331U};  // SGS1
constexpr std::size_t kHeaderBytes{8U};
constexpr std::size_t kDigestBytes{32U};
constexpr std::size_t kWifiPayloadMax{100U};
constexpr std::size_t kTokenPayloadMax{130U};

std::size_t bounded_length(const char* value, std::size_t maximum) noexcept {
  if (value == nullptr) return maximum + 1U;
  std::size_t length{0U};
  while (length <= maximum && value[length] != '\0') ++length;
  return length;
}

void put_u16(uint8_t* bytes, std::size_t offset, uint16_t value) noexcept {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1U] = static_cast<uint8_t>(value >> 8U);
}

uint16_t get_u16(const uint8_t* bytes, std::size_t offset) noexcept {
  return static_cast<uint16_t>(bytes[offset]) |
         static_cast<uint16_t>(bytes[offset + 1U] << 8U);
}

void put_u32(uint8_t* bytes, uint32_t value) noexcept {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8U);
  bytes[2] = static_cast<uint8_t>(value >> 16U);
  bytes[3] = static_cast<uint8_t>(value >> 24U);
}

uint32_t get_u32(const uint8_t* bytes) noexcept {
  return static_cast<uint32_t>(bytes[0]) |
         (static_cast<uint32_t>(bytes[1]) << 8U) |
         (static_cast<uint32_t>(bytes[2]) << 16U) |
         (static_cast<uint32_t>(bytes[3]) << 24U);
}

std::array<uint8_t, kDigestBytes> digest(const uint8_t* bytes,
                                          std::size_t size) noexcept {
  sigap::telemetry_contract::Sha256 sha;
  sha.update(std::string_view(reinterpret_cast<const char*>(bytes), size));
  return sha.final();
}

void append_digest(EncodedSecretBlob& output, std::size_t content_size) noexcept {
  const auto value = digest(output.bytes.data(), content_size);
  std::copy(value.begin(), value.end(), output.bytes.begin() + content_size);
  output.size = content_size + kDigestBytes;
}

bool digest_matches(const uint8_t* bytes, std::size_t content_size,
                    std::size_t total_size) noexcept {
  if (total_size != content_size + kDigestBytes) return false;
  const auto expected = digest(bytes, content_size);
  return std::equal(expected.begin(), expected.end(), bytes + content_size);
}

bool encode_strings(SecretSlot slot, const char* first, const char* second,
                    EncodedSecretBlob& output) noexcept {
  output = {};
  const std::size_t first_max = slot == SecretSlot::Wifi ? 32U : 128U;
  const std::size_t first_size = bounded_length(first, first_max);
  if (first_size == 0U || first_size > first_max) return false;
  std::size_t payload{2U + first_size};
  if (slot == SecretSlot::Wifi) {
    const std::size_t second_size = bounded_length(second, 64U);
    if (second_size == 0U || second_size > 64U) return false;
    payload += 2U + second_size;
  }
  const std::size_t content_size = kHeaderBytes + payload;
  if (content_size + kDigestBytes > output.bytes.size()) return false;
  put_u32(output.bytes.data(), kSecretMagic);
  output.bytes[4] = kSecretSchemaVersion;
  output.bytes[5] = static_cast<uint8_t>(slot);
  put_u16(output.bytes.data(), 6U, static_cast<uint16_t>(payload));
  uint8_t* cursor = output.bytes.data() + kHeaderBytes;
  put_u16(cursor, 0U, static_cast<uint16_t>(first_size));
  std::copy_n(reinterpret_cast<const uint8_t*>(first), first_size, cursor + 2U);
  cursor += 2U + first_size;
  if (slot == SecretSlot::Wifi) {
    const std::size_t second_size = bounded_length(second, 64U);
    put_u16(cursor, 0U, static_cast<uint16_t>(second_size));
    std::copy_n(reinterpret_cast<const uint8_t*>(second), second_size, cursor + 2U);
  }
  append_digest(output, content_size);
  return true;
}

SecretStoreResult decode_wifi_blob(const uint8_t* bytes, std::size_t size,
                                   NetworkSecrets& output) noexcept {
  secure_zero(output.ssid.data(), output.ssid.size());
  secure_zero(output.password.data(), output.password.size());
  if (bytes == nullptr || size < kHeaderBytes + kDigestBytes ||
      get_u32(bytes) != kSecretMagic) {
    return SecretStoreResult::Corrupt;
  }
  if (bytes[4] != kSecretSchemaVersion) return SecretStoreResult::UnsupportedSchema;
  if (bytes[5] != static_cast<uint8_t>(SecretSlot::Wifi)) return SecretStoreResult::Corrupt;
  const std::size_t payload = get_u16(bytes, 6U);
  if (payload < 4U || payload > kWifiPayloadMax ||
      size != kHeaderBytes + payload + kDigestBytes ||
      !digest_matches(bytes, kHeaderBytes + payload, size)) {
    return SecretStoreResult::Corrupt;
  }
  const uint8_t* cursor = bytes + kHeaderBytes;
  const std::size_t ssid_size = get_u16(cursor, 0U);
  if (ssid_size == 0U || ssid_size > 32U || 2U + ssid_size + 2U > payload) {
    return SecretStoreResult::Corrupt;
  }
  std::copy_n(reinterpret_cast<const char*>(cursor + 2U), ssid_size,
              output.ssid.data());
  cursor += 2U + ssid_size;
  const std::size_t password_size = get_u16(cursor, 0U);
  if (password_size == 0U || password_size > 64U ||
      2U + ssid_size + 2U + password_size != payload) {
    secure_zero(output.ssid.data(), output.ssid.size());
    return SecretStoreResult::Corrupt;
  }
  std::copy_n(reinterpret_cast<const char*>(cursor + 2U), password_size,
              output.password.data());
  return SecretStoreResult::Ok;
}

SecretStoreResult decode_token_blob(const uint8_t* bytes, std::size_t size,
                                    NetworkSecrets& output) noexcept {
  secure_zero(output.device_token.data(), output.device_token.size());
  if (bytes == nullptr || size < kHeaderBytes + kDigestBytes ||
      get_u32(bytes) != kSecretMagic) {
    return SecretStoreResult::Corrupt;
  }
  if (bytes[4] != kSecretSchemaVersion) return SecretStoreResult::UnsupportedSchema;
  if (bytes[5] != static_cast<uint8_t>(SecretSlot::DeviceToken)) {
    return SecretStoreResult::Corrupt;
  }
  const std::size_t payload = get_u16(bytes, 6U);
  if (payload < 2U || payload > kTokenPayloadMax ||
      size != kHeaderBytes + payload + kDigestBytes ||
      !digest_matches(bytes, kHeaderBytes + payload, size)) {
    return SecretStoreResult::Corrupt;
  }
  const std::size_t token_size = get_u16(bytes, kHeaderBytes);
  if (token_size == 0U || token_size > 128U || token_size + 2U != payload) {
    return SecretStoreResult::Corrupt;
  }
  std::copy_n(reinterpret_cast<const char*>(bytes + kHeaderBytes + 2U), token_size,
              output.device_token.data());
  return SecretStoreResult::Ok;
}

}  // namespace

bool SecretBlobCodec::encode_wifi(const char* ssid, const char* password,
                                  EncodedSecretBlob& output) noexcept {
  return encode_strings(SecretSlot::Wifi, ssid, password, output);
}

bool SecretBlobCodec::encode_token(const char* token,
                                   EncodedSecretBlob& output) noexcept {
  return encode_strings(SecretSlot::DeviceToken, token, nullptr, output);
}

SecretStoreResult SecretBlobCodec::decode_wifi(const uint8_t* bytes,
                                               std::size_t size,
                                               NetworkSecrets& output) noexcept {
  return decode_wifi_blob(bytes, size, output);
}

SecretStoreResult SecretBlobCodec::decode_token(const uint8_t* bytes,
                                                std::size_t size,
                                                NetworkSecrets& output) noexcept {
  return decode_token_blob(bytes, size, output);
}

bool PersistentSecretStore::read_slot(SecretSlot slot,
                                      NetworkSecrets& output) const noexcept {
  EncodedSecretBlob blob{};
  std::size_t size{0U};
  const auto read_status = backend_.read(slot, blob.bytes.data(), blob.bytes.size(), size);
  if (read_status == SecretBlobReadStatus::NotFound) {
    set_result(SecretStoreResult::NotFound);
    secure_zero(blob.bytes.data(), blob.bytes.size());
    return false;
  }
  if (read_status == SecretBlobReadStatus::Error) {
    set_result(SecretStoreResult::StorageError);
    secure_zero(blob.bytes.data(), blob.bytes.size());
    return false;
  }
  if (read_status == SecretBlobReadStatus::Invalid) {
    set_result(SecretStoreResult::Corrupt);
    secure_zero(blob.bytes.data(), blob.bytes.size());
    return false;
  }
  const auto result = slot == SecretSlot::Wifi
                          ? SecretBlobCodec::decode_wifi(blob.bytes.data(), size, output)
                          : SecretBlobCodec::decode_token(blob.bytes.data(), size, output);
  set_result(result);
  secure_zero(blob.bytes.data(), blob.bytes.size());
  return result == SecretStoreResult::Ok;
}

bool PersistentSecretStore::load_for_network(NetworkSecrets& output) {
  secure_zero(&output, sizeof(output));
  NetworkSecrets candidate{};
  if (!read_slot(SecretSlot::Wifi, candidate)) {
    secure_zero(&candidate, sizeof(candidate));
    return false;
  }
  if (!read_slot(SecretSlot::DeviceToken, candidate)) {
    secure_zero(&candidate, sizeof(candidate));
    return false;
  }
  output = candidate;
  secure_zero(&candidate, sizeof(candidate));
  set_result(SecretStoreResult::Ok);
  return true;
}

SecretPresence PersistentSecretStore::status() const noexcept {
  SecretPresence presence{};
  NetworkSecrets ignored{};
  presence.wifi_configured = read_slot(SecretSlot::Wifi, ignored);
  const SecretStoreResult wifi_result = result_;
  secure_zero(&ignored, sizeof(ignored));
  presence.device_token_configured = read_slot(SecretSlot::DeviceToken, ignored);
  const SecretStoreResult token_result = result_;
  secure_zero(&ignored, sizeof(ignored));
  if (wifi_result != SecretStoreResult::Ok) {
    set_result(wifi_result);
  } else {
    set_result(token_result);
  }
  return presence;
}

bool PersistentSecretStore::set_wifi(const char* ssid, const char* password) {
  EncodedSecretBlob blob{};
  if (!SecretBlobCodec::encode_wifi(ssid, password, blob)) {
    set_result(SecretStoreResult::InvalidInput);
    secure_zero(blob.bytes.data(), blob.bytes.size());
    return false;
  }
  const bool written = backend_.write(SecretSlot::Wifi, blob.bytes.data(), blob.size);
  secure_zero(blob.bytes.data(), blob.bytes.size());
  if (!written) {
    set_result(SecretStoreResult::StorageError);
    return false;
  }
  NetworkSecrets verified{};
  const bool readback = read_slot(SecretSlot::Wifi, verified);
  const std::size_t ssid_size = bounded_length(ssid, 32U);
  const std::size_t password_size = bounded_length(password, 64U);
  const bool matches = readback && ssid_size <= 32U && password_size <= 64U &&
      std::string_view(verified.ssid.data(), ssid_size) ==
          std::string_view(ssid, ssid_size) &&
      std::string_view(verified.password.data(), password_size) ==
          std::string_view(password, password_size);
  secure_zero(&verified, sizeof(verified));
  set_result(matches ? SecretStoreResult::Ok : SecretStoreResult::StorageError);
  return matches;
}

bool PersistentSecretStore::rotate_device_token(const char* token) {
  EncodedSecretBlob blob{};
  if (!SecretBlobCodec::encode_token(token, blob)) {
    set_result(SecretStoreResult::InvalidInput);
    secure_zero(blob.bytes.data(), blob.bytes.size());
    return false;
  }
  const bool written = backend_.write(SecretSlot::DeviceToken, blob.bytes.data(), blob.size);
  secure_zero(blob.bytes.data(), blob.bytes.size());
  set_result(written ? SecretStoreResult::Ok : SecretStoreResult::StorageError);
  return written;
}

bool PersistentSecretStore::clear_all(bool physical_confirmation) {
  if (!physical_confirmation) {
    set_result(SecretStoreResult::ConfirmationRequired);
    return false;
  }
  const bool cleared = backend_.clear_all();
  set_result(cleared ? SecretStoreResult::Ok : SecretStoreResult::StorageError);
  return cleared;
}

SecretBlobReadStatus MemorySecretBlobStore::read(
    SecretSlot slot, uint8_t* output, std::size_t capacity,
    std::size_t& size) const noexcept {
  if (output == nullptr || capacity < kMaxSecretBlobBytes) {
    size = 0U;
    return SecretBlobReadStatus::Invalid;
  }
  auto* self = const_cast<MemorySecretBlobStore*>(this);
  const Fault fault = slot == SecretSlot::Wifi ? Fault::ReadWifi : Fault::ReadToken;
  if (self->fault_pending_ && self->next_fault_ == fault) {
    self->fault_pending_ = false;
    size = 0U;
    return SecretBlobReadStatus::Error;
  }
  const std::size_t slot_index = index(slot);
  if (!present_[slot_index]) {
    size = 0U;
    return SecretBlobReadStatus::NotFound;
  }
  size = sizes_[slot_index];
  std::copy_n(blobs_[slot_index].data(), size, output);
  return SecretBlobReadStatus::Found;
}

bool MemorySecretBlobStore::write(SecretSlot slot, const uint8_t* bytes,
                                  std::size_t size) noexcept {
  if (bytes == nullptr || size == 0U || size > kMaxSecretBlobBytes) return false;
  const Fault fault = slot == SecretSlot::Wifi ? Fault::WriteWifi : Fault::WriteToken;
  if (fault_pending_ && next_fault_ == fault) {
    fault_pending_ = false;
    return false;
  }
  const std::size_t slot_index = index(slot);
  std::array<uint8_t, kMaxSecretBlobBytes> candidate{};
  std::copy_n(bytes, size, candidate.data());
  blobs_[slot_index] = candidate;
  sizes_[slot_index] = size;
  present_[slot_index] = true;
  secure_zero(candidate.data(), candidate.size());
  return true;
}

bool MemorySecretBlobStore::clear_all() noexcept {
  if (fault_pending_ && next_fault_ == Fault::Clear) {
    fault_pending_ = false;
    return false;
  }
  for (auto& blob : blobs_) secure_zero(blob.data(), blob.size());
  sizes_.fill(0U);
  present_.fill(false);
  return true;
}

void MemorySecretBlobStore::fail_next(Fault fault) noexcept {
  next_fault_ = fault;
  fault_pending_ = true;
}

void MemorySecretBlobStore::corrupt(SecretSlot slot) noexcept {
  const std::size_t slot_index = index(slot);
  if (present_[slot_index] && sizes_[slot_index] != 0U) blobs_[slot_index][0] ^= 0xffU;
}

bool MemorySecretBlobStore::present(SecretSlot slot) const noexcept {
  return present_[index(slot)];
}

#ifdef ARDUINO

namespace {
constexpr const char* kNamespace{"sigap_secret"};
const char* key_for_slot(SecretSlot slot) noexcept {
  return slot == SecretSlot::Wifi ? "wifi" : "token";
}
}  // namespace

SecretBlobReadStatus NvsSecretBlobStore::read(
    SecretSlot slot, uint8_t* output, std::size_t capacity,
    std::size_t& size) const noexcept {
  size = 0U;
  if (output == nullptr || capacity < kMaxSecretBlobBytes) return SecretBlobReadStatus::Invalid;
  nvs_handle_t handle{};
  const esp_err_t open_result = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (open_result == ESP_ERR_NVS_NOT_FOUND) return SecretBlobReadStatus::NotFound;
  if (open_result != ESP_OK) return SecretBlobReadStatus::Error;
  std::size_t required{0U};
  esp_err_t result = nvs_get_blob(handle, key_for_slot(slot), nullptr, &required);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return SecretBlobReadStatus::NotFound;
  }
  if (result != ESP_OK || required == 0U || required > capacity) {
    nvs_close(handle);
    return required > capacity ? SecretBlobReadStatus::Invalid
                               : SecretBlobReadStatus::Error;
  }
  result = nvs_get_blob(handle, key_for_slot(slot), output, &required);
  nvs_close(handle);
  if (result != ESP_OK) return SecretBlobReadStatus::Error;
  size = required;
  return SecretBlobReadStatus::Found;
}

bool NvsSecretBlobStore::write(SecretSlot slot, const uint8_t* bytes,
                               std::size_t size) noexcept {
  if (bytes == nullptr || size == 0U || size > kMaxSecretBlobBytes) return false;
  nvs_handle_t handle{};
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  const esp_err_t set_result = nvs_set_blob(handle, key_for_slot(slot), bytes, size);
  const esp_err_t commit_result = set_result == ESP_OK ? nvs_commit(handle) : set_result;
  nvs_close(handle);
  return commit_result == ESP_OK;
}

bool NvsSecretBlobStore::clear_all() noexcept {
  nvs_handle_t handle{};
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  esp_err_t result = nvs_erase_key(handle, "wifi");
  if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return false;
  }
  result = nvs_erase_key(handle, "token");
  if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return false;
  }
  const esp_err_t commit_result = nvs_commit(handle);
  nvs_close(handle);
  return commit_result == ESP_OK;
}

#else

SecretBlobReadStatus NvsSecretBlobStore::read(
    SecretSlot, uint8_t*, std::size_t, std::size_t& size) const noexcept {
  size = 0U;
  return SecretBlobReadStatus::Error;
}

bool NvsSecretBlobStore::write(SecretSlot, const uint8_t*, std::size_t) noexcept {
  return false;
}

bool NvsSecretBlobStore::clear_all() noexcept { return false; }

#endif

}  // namespace sigap::provisioning
