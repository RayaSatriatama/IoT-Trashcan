#include "nvs_config_store.h"

namespace sigap::config {

#ifdef ARDUINO

namespace {
constexpr const char* kMetadataNamespace{"sigap_cfg_meta"};
constexpr const char* kActiveKey{"active"};
constexpr const char* kProvisionedKey{"provisioned"};
constexpr const char* kBlobKey{"blob"};
}

const char* NvsConfigStore::slot_namespace(ConfigSlot slot) noexcept {
  return slot == ConfigSlot::A ? "sigap_cfg_a" : "sigap_cfg_b";
}

bool NvsConfigStore::read_active_pointer(ConfigSlot& slot) const noexcept {
  if (!preferences_.begin(kMetadataNamespace, true)) return false;
  const uint8_t value = preferences_.getUChar(kActiveKey, 0xffU);
  preferences_.end();
  if (value > 1U) return false;
  slot = value == 0U ? ConfigSlot::A : ConfigSlot::B;
  return true;
}

bool NvsConfigStore::read_slot(ConfigSlot slot,
                               EncodedConfig& encoded) const noexcept {
  if (!preferences_.begin(slot_namespace(slot), true)) return false;
  const std::size_t size = preferences_.getBytesLength(kBlobKey);
  if (size != kEncodedConfigBytes) {
    preferences_.end();
    return false;
  }
  const std::size_t read =
      preferences_.getBytes(kBlobKey, encoded.bytes.data(), encoded.bytes.size());
  preferences_.end();
  if (read != kEncodedConfigBytes) return false;
  encoded.size = read;
  return true;
}

bool NvsConfigStore::write_slot(ConfigSlot slot,
                                const EncodedConfig& encoded) noexcept {
  if (encoded.size != kEncodedConfigBytes ||
      !preferences_.begin(slot_namespace(slot), false)) {
    return false;
  }
  const std::size_t written =
      preferences_.putBytes(kBlobKey, encoded.bytes.data(), encoded.size);
  preferences_.end();
  return written == encoded.size;
}

bool NvsConfigStore::write_active_pointer(ConfigSlot slot) noexcept {
  if (!preferences_.begin(kMetadataNamespace, false)) return false;
  const std::size_t written = preferences_.putUChar(
      kActiveKey, slot == ConfigSlot::A ? static_cast<uint8_t>(0)
                                        : static_cast<uint8_t>(1));
  preferences_.end();
  return written == sizeof(uint8_t);
}

bool NvsConfigStore::is_provisioned() const noexcept {
  if (!preferences_.begin(kMetadataNamespace, true)) return false;
  const bool value = preferences_.getBool(kProvisionedKey, false);
  preferences_.end();
  return value;
}

bool NvsConfigStore::mark_provisioned() noexcept {
  if (!preferences_.begin(kMetadataNamespace, false)) return false;
  const bool written = preferences_.putBool(kProvisionedKey, true);
  preferences_.end();
  return written;
}

#else

bool NvsConfigStore::read_active_pointer(ConfigSlot&) const noexcept {
  return false;
}

bool NvsConfigStore::read_slot(ConfigSlot, EncodedConfig&) const noexcept {
  return false;
}

bool NvsConfigStore::write_slot(ConfigSlot,
                                const EncodedConfig&) noexcept {
  return false;
}

bool NvsConfigStore::write_active_pointer(ConfigSlot) noexcept {
  return false;
}

bool NvsConfigStore::is_provisioned() const noexcept { return false; }

bool NvsConfigStore::mark_provisioned() noexcept { return false; }

#endif

}  // namespace sigap::config
