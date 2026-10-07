#include "models/model_lifecycle.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

#include "../../../telemetry_contract/src/telemetry/sha256.h"

#ifdef ARDUINO
#include <nvs.h>
#endif

namespace sigap::models {
namespace {

using telemetry_contract::sha256;

void put_u32_le(uint8_t* bytes, std::size_t offset, uint32_t value) noexcept {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1U] = static_cast<uint8_t>(value >> 8U);
  bytes[offset + 2U] = static_cast<uint8_t>(value >> 16U);
  bytes[offset + 3U] = static_cast<uint8_t>(value >> 24U);
}

uint32_t get_u32_le(const uint8_t* bytes, std::size_t offset) noexcept {
  return static_cast<uint32_t>(bytes[offset]) |
         (static_cast<uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<uint32_t>(bytes[offset + 3U]) << 24U);
}

config::ConfigHash metadata_integrity(
    const ModelActivationMetadata& metadata) noexcept {
  std::array<uint8_t, kModelActivationCoreBytes> bytes{};
  bytes[0] = kModelActivationMetadataFormatVersion;
  put_u32_le(bytes.data(), kModelActivationGenerationOffset,
             metadata.generation);
  bytes[kModelActivationHasActiveOffset] = metadata.has_active ? 1U : 0U;
  bytes[kModelActivationActiveSlotOffset] =
      static_cast<uint8_t>(metadata.active_slot);
  put_u32_le(bytes.data(), kModelActivationActiveVersionOffset,
             metadata.active_version);
  std::copy(metadata.active_hash.begin(), metadata.active_hash.end(),
            bytes.begin() + kModelActivationActiveHashOffset);
  bytes[kModelActivationHasPreviousOffset] = metadata.has_previous ? 1U : 0U;
  bytes[kModelActivationPreviousSlotOffset] =
      static_cast<uint8_t>(metadata.previous_slot);
  put_u32_le(bytes.data(), kModelActivationPreviousVersionOffset,
             metadata.previous_version);
  std::copy(metadata.previous_hash.begin(), metadata.previous_hash.end(),
            bytes.begin() + kModelActivationPreviousHashOffset);
  return sha256(std::string_view(
      reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

bool metadata_integrity_valid(
    const ModelActivationMetadata& metadata) noexcept {
  return metadata.integrity == metadata_integrity(metadata);
}

void encode_metadata(
    const ModelActivationMetadata& metadata,
    std::array<uint8_t, kModelActivationMetadataBytes>& bytes) noexcept {
  bytes.fill(0U);
  bytes[0] = kModelActivationMetadataFormatVersion;
  put_u32_le(bytes.data(), kModelActivationGenerationOffset,
             metadata.generation);
  bytes[kModelActivationHasActiveOffset] = metadata.has_active ? 1U : 0U;
  bytes[kModelActivationActiveSlotOffset] =
      static_cast<uint8_t>(metadata.active_slot);
  put_u32_le(bytes.data(), kModelActivationActiveVersionOffset,
             metadata.active_version);
  std::copy(metadata.active_hash.begin(), metadata.active_hash.end(),
            bytes.begin() + kModelActivationActiveHashOffset);
  bytes[kModelActivationHasPreviousOffset] = metadata.has_previous ? 1U : 0U;
  bytes[kModelActivationPreviousSlotOffset] =
      static_cast<uint8_t>(metadata.previous_slot);
  put_u32_le(bytes.data(), kModelActivationPreviousVersionOffset,
             metadata.previous_version);
  std::copy(metadata.previous_hash.begin(), metadata.previous_hash.end(),
            bytes.begin() + kModelActivationPreviousHashOffset);
  const auto integrity = metadata_integrity(metadata);
  std::copy(integrity.begin(), integrity.end(),
            bytes.begin() + kModelActivationIntegrityOffset);
}

bool decode_metadata(const uint8_t* bytes, std::size_t size,
                     ModelActivationMetadata& metadata) noexcept {
  if (bytes == nullptr || size != kModelActivationMetadataBytes ||
      bytes[0] != kModelActivationMetadataFormatVersion ||
      bytes[kModelActivationHasActiveOffset] > 1U ||
      bytes[kModelActivationHasPreviousOffset] > 1U ||
      bytes[kModelActivationActiveSlotOffset] >
          static_cast<uint8_t>(ModelSlot::B) ||
      bytes[kModelActivationPreviousSlotOffset] >
          static_cast<uint8_t>(ModelSlot::B)) {
    return false;
  }
  metadata = ModelActivationMetadata{};
  metadata.generation = get_u32_le(bytes, kModelActivationGenerationOffset);
  metadata.has_active = bytes[kModelActivationHasActiveOffset] != 0U;
  metadata.active_slot =
      static_cast<ModelSlot>(bytes[kModelActivationActiveSlotOffset]);
  metadata.active_version =
      get_u32_le(bytes, kModelActivationActiveVersionOffset);
  std::copy_n(bytes + kModelActivationActiveHashOffset,
              metadata.active_hash.size(), metadata.active_hash.begin());
  metadata.has_previous = bytes[kModelActivationHasPreviousOffset] != 0U;
  metadata.previous_slot =
      static_cast<ModelSlot>(bytes[kModelActivationPreviousSlotOffset]);
  metadata.previous_version =
      get_u32_le(bytes, kModelActivationPreviousVersionOffset);
  std::copy_n(bytes + kModelActivationPreviousHashOffset,
              metadata.previous_hash.size(), metadata.previous_hash.begin());
  std::copy_n(bytes + kModelActivationIntegrityOffset,
              metadata.integrity.size(), metadata.integrity.begin());
  return metadata_integrity_valid(metadata);
}

acquisition::ProfileRef to_ref(const ModelProfileIdentity& identity) noexcept {
  acquisition::ProfileRef ref{};
  ref.kind = identity.key.kind;
  ref.schema_version = identity.schema_version;
  ref.profile_version = identity.profile_version;
  ref.target_id = static_cast<uint32_t>(identity.key.target);
  ref.hash = identity.hash;
  ref.present = true;
  return ref;
}

std::size_t model_key_index(ModelProfileKey key) noexcept {
  if (key.kind == acquisition::ProfileKind::Fis) {
    if (key.target == ModelTarget::FisOrganic) return 0U;
    if (key.target == ModelTarget::FisInorganic) return 1U;
    if (key.target == ModelTarget::FisB3) return 2U;
  }
  if (key.kind == acquisition::ProfileKind::Safety &&
      key.target == ModelTarget::SafetyB3) {
    return 3U;
  }
  return 4U;
}

bool valid_slot(ModelSlot slot) noexcept {
  return slot == ModelSlot::A || slot == ModelSlot::B;
}

#ifdef ARDUINO
constexpr const char* kModelNamespace{"sigap_model"};

bool model_storage_key(char* output, std::size_t capacity,
                       ModelProfileKey key, ModelSlot slot) noexcept {
  if (output == nullptr || capacity < 10U || !valid_model_key(key) ||
      !valid_slot(slot)) {
    return false;
  }
  const char family = key.kind == acquisition::ProfileKind::Fis ? 'f' : 's';
  const int written = std::snprintf(
      output, capacity, "%c_%c%03u", slot == ModelSlot::A ? 'a' : 'b',
      family, static_cast<unsigned>(key.target));
  return written > 0 && static_cast<std::size_t>(written) < capacity;
}

bool model_metadata_key(char* output, std::size_t capacity,
                        ModelProfileKey key, std::size_t copy) noexcept {
  if (output == nullptr || capacity < 12U || !valid_model_key(key) ||
      copy >= kModelActivationCopyCount) {
    return false;
  }
  const char family = key.kind == acquisition::ProfileKind::Fis ? 'f' : 's';
  const int written = std::snprintf(output, capacity, "m_%c%03u_%u", family,
                                    static_cast<unsigned>(key.target),
                                    static_cast<unsigned>(copy));
  return written > 0 && static_cast<std::size_t>(written) < capacity;
}
#endif

}  // namespace

const ModelProfileCodec& ModelProfilePersistence::default_codec() noexcept {
  static const ModelProfileCodec codec{};
  return codec;
}

bool ModelProfilePersistence::load_committed(
    ModelProfileKey key, ModelSlot slot, uint32_t version,
    const config::ConfigHash& hash, EncodedModelProfile& output) const noexcept {
  if (!valid_slot(slot) ||
      store_.read_slot(key, slot, output) != ModelLoadStatus::Found ||
      !codec_.validate(output, key) || output.hash() != hash) {
    return false;
  }
  ModelProfileIdentity identity{};
  return codec_.identity(output, identity) && identity.key == key &&
         identity.profile_version == version;
}

ModelLoadStatus ModelProfilePersistence::load(
    ModelProfileKey key, EncodedModelProfile& output) const noexcept {
  if (!valid_model_key(key)) return ModelLoadStatus::Error;
  ModelActivationMetadata metadata{};
  const auto metadata_status = store_.read_activation(key, metadata);
  if (metadata_status != ModelLoadStatus::Found) return metadata_status;
  if (!metadata.has_active) return ModelLoadStatus::NotFound;
  if (load_committed(key, metadata.active_slot, metadata.active_version,
                     metadata.active_hash, output)) {
    return ModelLoadStatus::Found;
  }
  if (metadata.has_previous &&
      load_committed(key, metadata.previous_slot, metadata.previous_version,
                     metadata.previous_hash, output)) {
    return ModelLoadStatus::Found;
  }
  return ModelLoadStatus::Invalid;
}

ModelCommitResult ModelProfilePersistence::commit(
    ModelProfileKey key, const EncodedModelProfile& candidate) noexcept {
  if (!valid_model_key(key) || !codec_.validate(candidate, key)) {
    return ModelCommitResult::Invalid;
  }
  ModelProfileIdentity candidate_identity{};
  if (!codec_.identity(candidate, candidate_identity) ||
      !(candidate_identity.key == key)) {
    return ModelCommitResult::Invalid;
  }
  ModelActivationMetadata old{};
  const auto metadata_status = store_.read_activation(key, old);
  if (metadata_status == ModelLoadStatus::Error ||
      metadata_status == ModelLoadStatus::Invalid) {
    return ModelCommitResult::ActivationFailed;
  }

  EncodedModelProfile current{};
  ModelProfileIdentity current_identity{};
  bool have_current = false;
  ModelSlot current_slot{ModelSlot::A};
  ModelSlot destination{ModelSlot::A};
  if (metadata_status == ModelLoadStatus::Found) {
    if (!old.has_active || old.active_version == 0U ||
        candidate_identity.profile_version <= old.active_version) {
      return ModelCommitResult::Invalid;
    }
    if (load_committed(key, old.active_slot, old.active_version, old.active_hash,
                       current)) {
      current_slot = old.active_slot;
      destination = other_model_slot(current_slot);
      have_current = codec_.identity(current, current_identity);
    } else if (old.has_previous &&
               load_committed(key, old.previous_slot, old.previous_version,
                              old.previous_hash, current)) {
      current_slot = old.previous_slot;
      destination = old.active_slot;
      have_current = codec_.identity(current, current_identity);
    }
    if (!have_current) return ModelCommitResult::ActivationFailed;
  }

  if (!store_.write_slot(key, destination, candidate)) {
    return ModelCommitResult::WriteFailed;
  }
  EncodedModelProfile readback{};
  if (store_.read_slot(key, destination, readback) != ModelLoadStatus::Found) {
    return ModelCommitResult::ReadbackFailed;
  }
  if (!codec_.validate(readback, key) || readback.hash() != candidate.hash()) {
    return ModelCommitResult::ReadbackFailed;
  }

  ModelActivationMetadata next{};
  next.generation = metadata_status == ModelLoadStatus::Found ? old.generation : 0U;
  if (next.generation == std::numeric_limits<uint32_t>::max()) {
    return ModelCommitResult::ActivationFailed;
  }
  next.has_active = true;
  next.active_slot = destination;
  next.active_version = candidate_identity.profile_version;
  next.active_hash = candidate.hash();
  if (have_current) {
    next.has_previous = true;
    next.previous_slot = current_slot;
    next.previous_version = current_identity.profile_version;
    next.previous_hash = current.hash();
  }
  return store_.write_activation(key, next) ? ModelCommitResult::Committed
                                             : ModelCommitResult::ActivationFailed;
}

ModelLoadStatus ModelProfilePersistence::rollback(ModelProfileKey key) noexcept {
  if (!valid_model_key(key)) return ModelLoadStatus::Error;
  ModelActivationMetadata metadata{};
  if (store_.read_activation(key, metadata) != ModelLoadStatus::Found ||
      !metadata.has_active || !metadata.has_previous) {
    return ModelLoadStatus::NotFound;
  }
  EncodedModelProfile active{};
  EncodedModelProfile previous{};
  if (!load_committed(key, metadata.active_slot, metadata.active_version,
                      metadata.active_hash, active) ||
      !load_committed(key, metadata.previous_slot, metadata.previous_version,
                      metadata.previous_hash, previous)) {
    return ModelLoadStatus::Invalid;
  }
  ModelActivationMetadata next{};
  next.generation = metadata.generation;
  next.has_active = true;
  next.active_slot = metadata.previous_slot;
  next.active_version = metadata.previous_version;
  next.active_hash = metadata.previous_hash;
  next.has_previous = true;
  next.previous_slot = metadata.active_slot;
  next.previous_version = metadata.active_version;
  next.previous_hash = metadata.active_hash;
  return store_.write_activation(key, next) ? ModelLoadStatus::Found
                                             : ModelLoadStatus::Error;
}

std::size_t MemoryModelSlotStore::key_index(ModelProfileKey key) noexcept {
  return model_key_index(key);
}

ModelLoadStatus MemoryModelSlotStore::read_activation(
    ModelProfileKey key, ModelActivationMetadata& metadata) const noexcept {
  if (fault_armed_ && pending_fault_ == Fault::ReadActivation) {
    fault_armed_ = false;
    return ModelLoadStatus::Error;
  }
  const auto index = key_index(key);
  if (index >= kKeyCount) return ModelLoadStatus::Error;
  bool found = false;
  bool invalid = false;
  ModelActivationMetadata selected{};
  for (std::size_t copy = 0U; copy < kModelActivationCopyCount; ++copy) {
    if (!metadata_present_[index][copy]) continue;
    if (!metadata_integrity_valid(metadata_[index][copy])) {
      invalid = true;
      continue;
    }
    if (!found || metadata_[index][copy].generation > selected.generation) {
      selected = metadata_[index][copy];
      found = true;
    }
  }
  if (!found) return invalid ? ModelLoadStatus::Invalid : ModelLoadStatus::NotFound;
  metadata = selected;
  return ModelLoadStatus::Found;
}

ModelLoadStatus MemoryModelSlotStore::read_slot(
    ModelProfileKey key, ModelSlot slot, EncodedModelProfile& encoded) const noexcept {
  if (fault_armed_ && pending_fault_ == Fault::ReadSlot) {
    fault_armed_ = false;
    return ModelLoadStatus::Error;
  }
  const auto key_i = key_index(key);
  if (key_i >= kKeyCount || !valid_slot(slot)) return ModelLoadStatus::Error;
  const auto slot_i = slot_index(slot);
  if (!slot_present_[key_i][slot_i]) return ModelLoadStatus::NotFound;
  encoded = slots_[key_i][slot_i];
  return ModelLoadStatus::Found;
}

bool MemoryModelSlotStore::write_slot(
    ModelProfileKey key, ModelSlot slot,
    const EncodedModelProfile& encoded) noexcept {
  if (fault_armed_ && pending_fault_ == Fault::WriteSlot) {
    fault_armed_ = false;
    return false;
  }
  const auto key_i = key_index(key);
  if (key_i >= kKeyCount || !valid_slot(slot) || encoded.size == 0U ||
      encoded.size > encoded.bytes.size()) {
    return false;
  }
  const auto slot_i = slot_index(slot);
  slots_[key_i][slot_i] = encoded;
  slot_present_[key_i][slot_i] = true;
  if (fault_armed_ && pending_fault_ == Fault::CorruptAfterWrite) {
    slots_[key_i][slot_i].bytes[0] ^= 0x01U;
    fault_armed_ = false;
  }
  return true;
}

bool MemoryModelSlotStore::write_activation(
    ModelProfileKey key, const ModelActivationMetadata& metadata) noexcept {
  if (fault_armed_ && pending_fault_ == Fault::WriteActivation) {
    fault_armed_ = false;
    return false;
  }
  const auto index = key_index(key);
  if (index >= kKeyCount) return false;
  bool found = false;
  uint32_t newest_generation = 0U;
  std::size_t newest_copy = 0U;
  for (std::size_t copy = 0U; copy < kModelActivationCopyCount; ++copy) {
    if (!metadata_present_[index][copy] ||
        !metadata_integrity_valid(metadata_[index][copy])) continue;
    if (!found || metadata_[index][copy].generation > newest_generation) {
      newest_generation = metadata_[index][copy].generation;
      newest_copy = copy;
      found = true;
    }
  }
  if (found && newest_generation == std::numeric_limits<uint32_t>::max()) return false;
  const std::size_t destination = found ? (newest_copy == 0U ? 1U : 0U) : 0U;
  auto candidate = metadata;
  candidate.generation = found ? newest_generation + 1U : 1U;
  candidate.integrity = metadata_integrity(candidate);
  metadata_[index][destination] = candidate;
  metadata_present_[index][destination] = true;
  return true;
}

void MemoryModelSlotStore::fail_next(Fault fault) noexcept {
  pending_fault_ = fault;
  fault_armed_ = true;
}

void MemoryModelSlotStore::corrupt_slot(ModelProfileKey key, ModelSlot slot) noexcept {
  const auto key_i = key_index(key);
  if (key_i >= kKeyCount || !valid_slot(slot)) return;
  const auto slot_i = slot_index(slot);
  if (slot_present_[key_i][slot_i] && slots_[key_i][slot_i].size != 0U) {
    slots_[key_i][slot_i].bytes[0] ^= 0x01U;
  }
}

void MemoryModelSlotStore::corrupt_activation(ModelProfileKey key) noexcept {
  const auto index = key_index(key);
  if (index >= kKeyCount) return;
  for (std::size_t copy = 0U; copy < kModelActivationCopyCount; ++copy) {
    if (metadata_present_[index][copy]) metadata_[index][copy].active_hash[0] ^= 0x01U;
  }
}

#ifdef ARDUINO
ModelLoadStatus NvsModelSlotStore::read_activation(
    ModelProfileKey key, ModelActivationMetadata& metadata) const noexcept {
  if (!valid_model_key(key)) return ModelLoadStatus::Error;
  nvs_handle_t handle{};
  const auto opened = nvs_open(kModelNamespace, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return ModelLoadStatus::NotFound;
  if (opened != ESP_OK) return ModelLoadStatus::Error;
  bool found = false;
  bool invalid = false;
  ModelActivationMetadata selected{};
  for (std::size_t copy = 0U; copy < kModelActivationCopyCount; ++copy) {
    char storage_key[16]{};
    if (!model_metadata_key(storage_key, sizeof(storage_key), key, copy)) {
      nvs_close(handle);
      return ModelLoadStatus::Error;
    }
    std::array<uint8_t, kModelActivationMetadataBytes> bytes{};
    std::size_t size = bytes.size();
    const auto result = nvs_get_blob(handle, storage_key, bytes.data(), &size);
    if (result == ESP_ERR_NVS_NOT_FOUND) continue;
    if (result != ESP_OK) {
      nvs_close(handle);
      return ModelLoadStatus::Error;
    }
    ModelActivationMetadata candidate{};
    if (!decode_metadata(bytes.data(), size, candidate)) {
      invalid = true;
      continue;
    }
    if (!found || candidate.generation > selected.generation) {
      selected = candidate;
      found = true;
    }
  }
  nvs_close(handle);
  if (!found) return invalid ? ModelLoadStatus::Invalid : ModelLoadStatus::NotFound;
  metadata = selected;
  return ModelLoadStatus::Found;
}

ModelLoadStatus NvsModelSlotStore::read_slot(
    ModelProfileKey key, ModelSlot slot,
    EncodedModelProfile& encoded) const noexcept {
  if (!valid_model_key(key) || !valid_slot(slot)) return ModelLoadStatus::Error;
  nvs_handle_t handle{};
  const auto opened = nvs_open(kModelNamespace, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return ModelLoadStatus::NotFound;
  if (opened != ESP_OK) return ModelLoadStatus::Error;
  char storage_key[16]{};
  if (!model_storage_key(storage_key, sizeof(storage_key), key, slot)) {
    nvs_close(handle);
    return ModelLoadStatus::Error;
  }
  std::size_t size = encoded.bytes.size();
  const auto result = nvs_get_blob(handle, storage_key, encoded.bytes.data(), &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return ModelLoadStatus::NotFound;
  if (result != ESP_OK) return ModelLoadStatus::Error;
  if (size == 0U || size > encoded.bytes.size()) return ModelLoadStatus::Invalid;
  encoded.size = size;
  return ModelLoadStatus::Found;
}

bool NvsModelSlotStore::write_slot(
    ModelProfileKey key, ModelSlot slot,
    const EncodedModelProfile& encoded) noexcept {
  if (!valid_model_key(key) || !valid_slot(slot) || encoded.size == 0U ||
      encoded.size > encoded.bytes.size()) return false;
  nvs_handle_t handle{};
  if (nvs_open(kModelNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  char storage_key[16]{};
  if (!model_storage_key(storage_key, sizeof(storage_key), key, slot)) {
    nvs_close(handle);
    return false;
  }
  const auto set_result =
      nvs_set_blob(handle, storage_key, encoded.bytes.data(), encoded.size);
  const auto commit_result = set_result == ESP_OK ? nvs_commit(handle) : set_result;
  nvs_close(handle);
  return commit_result == ESP_OK;
}

bool NvsModelSlotStore::write_activation(
    ModelProfileKey key, const ModelActivationMetadata& metadata) noexcept {
  if (!valid_model_key(key)) return false;
  nvs_handle_t handle{};
  if (nvs_open(kModelNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  bool found = false;
  uint32_t newest_generation = 0U;
  std::size_t newest_copy = 0U;
  for (std::size_t copy = 0U; copy < kModelActivationCopyCount; ++copy) {
    char existing_key[16]{};
    if (!model_metadata_key(existing_key, sizeof(existing_key), key, copy)) {
      nvs_close(handle);
      return false;
    }
    std::array<uint8_t, kModelActivationMetadataBytes> bytes{};
    std::size_t size = bytes.size();
    const auto result = nvs_get_blob(handle, existing_key, bytes.data(), &size);
    if (result == ESP_ERR_NVS_NOT_FOUND) continue;
    if (result != ESP_OK) {
      nvs_close(handle);
      return false;
    }
    ModelActivationMetadata existing{};
    if (!decode_metadata(bytes.data(), size, existing)) continue;
    if (!found || existing.generation > newest_generation) {
      newest_generation = existing.generation;
      newest_copy = copy;
      found = true;
    }
  }
  if (found && newest_generation == std::numeric_limits<uint32_t>::max()) {
    nvs_close(handle);
    return false;
  }
  const std::size_t destination = found ? (newest_copy == 0U ? 1U : 0U) : 0U;
  auto candidate = metadata;
  candidate.generation = found ? newest_generation + 1U : 1U;
  candidate.integrity = metadata_integrity(candidate);
  std::array<uint8_t, kModelActivationMetadataBytes> bytes{};
  encode_metadata(candidate, bytes);
  char destination_key[16]{};
  if (!model_metadata_key(destination_key, sizeof(destination_key), key,
                          destination)) {
    nvs_close(handle);
    return false;
  }
  const auto set_result =
      nvs_set_blob(handle, destination_key, bytes.data(), bytes.size());
  const auto commit_result = set_result == ESP_OK ? nvs_commit(handle) : set_result;
  nvs_close(handle);
  return commit_result == ESP_OK;
}
#else
ModelLoadStatus NvsModelSlotStore::read_activation(
    ModelProfileKey, ModelActivationMetadata&) const noexcept {
  return ModelLoadStatus::Error;
}
ModelLoadStatus NvsModelSlotStore::read_slot(
    ModelProfileKey, ModelSlot, EncodedModelProfile&) const noexcept {
  return ModelLoadStatus::Error;
}
bool NvsModelSlotStore::write_slot(
    ModelProfileKey, ModelSlot, const EncodedModelProfile&) noexcept {
  return false;
}
bool NvsModelSlotStore::write_activation(
    ModelProfileKey, const ModelActivationMetadata&) noexcept {
  return false;
}
#endif

const ModelProfileCodec& FaultInjectableModelPersistence::default_codec() noexcept {
  return ModelProfilePersistence::default_codec();
}

ModelLoadStatus FaultInjectableModelPersistence::load(
    ModelProfileKey key, EncodedModelProfile& output) const noexcept {
  return persistence_.load(key, output);
}

ModelCommitResult FaultInjectableModelPersistence::commit(
    ModelProfileKey key, const EncodedModelProfile& candidate) noexcept {
  return persistence_.commit(key, candidate);
}

ModelLoadStatus FaultInjectableModelPersistence::rollback(
    ModelProfileKey key) noexcept {
  return persistence_.rollback(key);
}

void FaultInjectableModelPersistence::fail_next(
    ModelPersistenceFault fault) noexcept {
  switch (fault) {
    case ModelPersistenceFault::WriteCandidate:
      store_.fail_next(MemoryModelSlotStore::Fault::WriteSlot);
      break;
    case ModelPersistenceFault::CorruptAfterWrite:
      store_.fail_next(MemoryModelSlotStore::Fault::CorruptAfterWrite);
      break;
    case ModelPersistenceFault::Readback:
      store_.fail_next(MemoryModelSlotStore::Fault::ReadSlot);
      break;
    case ModelPersistenceFault::Activate:
      store_.fail_next(MemoryModelSlotStore::Fault::WriteActivation);
      break;
    case ModelPersistenceFault::ReadActivation:
      store_.fail_next(MemoryModelSlotStore::Fault::ReadActivation);
      break;
  }
}

void FaultInjectableModelPersistence::corrupt_active(
    ModelProfileKey key) noexcept {
  ModelActivationMetadata metadata{};
  if (store_.read_activation(key, metadata) == ModelLoadStatus::Found &&
      metadata.has_active) {
    store_.corrupt_slot(key, metadata.active_slot);
  }
}

void FaultInjectableModelPersistence::corrupt_activation(
    ModelProfileKey key) noexcept {
  store_.corrupt_activation(key);
}

std::size_t ModelProfileLifecycle::index(ModelProfileKey key) noexcept {
  return model_key_index(key);
}

bool ModelProfileLifecycle::load(ModelProfileKey key) noexcept {
  const auto entry_index = index(key);
  if (entry_index >= entries_.size()) return false;
  auto& entry = entries_[entry_index];
  EncodedModelProfile encoded{};
  const auto status = persistence_.load(key, encoded);
  entry = Entry{};
  entry.initialized = true;
  if (status == ModelLoadStatus::NotFound) return true;
  if (status != ModelLoadStatus::Found ||
      !codec_.identity(encoded, entry.active_identity) ||
      !(entry.active_identity.key == key) || !codec_.validate(encoded, key)) {
    entry.state = ModelLifecycleState::Error;
    return false;
  }
  entry.active = encoded;
  entry.active_present = true;
  entry.state = ModelLifecycleState::Active;
  return true;
}

bool ModelProfileLifecycle::stage(
    ModelProfileKey key, const EncodedModelProfile& encoded) noexcept {
  const auto entry_index = index(key);
  if (entry_index >= entries_.size() || !codec_.validate(encoded, key)) return false;
  auto& entry = entries_[entry_index];
  if (!entry.initialized) return false;
  ModelProfileIdentity candidate{};
  if (!codec_.identity(encoded, candidate) || !(candidate.key == key) ||
      (entry.active_present &&
       candidate.profile_version <= entry.active_identity.profile_version)) return false;
  entry.draft = encoded;
  entry.draft_present = true;
  entry.state = ModelLifecycleState::Draft;
  return true;
}

bool ModelProfileLifecycle::validate(ModelProfileKey key) noexcept {
  const auto entry_index = index(key);
  if (entry_index >= entries_.size()) return false;
  auto& entry = entries_[entry_index];
  if (!entry.draft_present || !codec_.validate(entry.draft, key)) return false;
  entry.state = ModelLifecycleState::Validated;
  return true;
}

bool ModelProfileLifecycle::preview(ModelProfileKey key) noexcept {
  const auto entry_index = index(key);
  if (entry_index >= entries_.size()) return false;
  auto& entry = entries_[entry_index];
  if (entry.state != ModelLifecycleState::Validated || !entry.draft_present) return false;
  entry.state = ModelLifecycleState::Preview;
  return true;
}

bool ModelProfileLifecycle::commit(ModelProfileKey key) noexcept {
  const auto entry_index = index(key);
  if (entry_index >= entries_.size()) return false;
  auto& entry = entries_[entry_index];
  if (entry.state != ModelLifecycleState::Preview || !entry.draft_present) return false;
  if (persistence_.commit(key, entry.draft) != ModelCommitResult::Committed) return false;
  ModelProfileIdentity identity{};
  if (!codec_.identity(entry.draft, identity)) {
    entry.state = ModelLifecycleState::Error;
    return false;
  }
  entry.active = entry.draft;
  entry.active_identity = identity;
  entry.active_present = true;
  entry.draft = EncodedModelProfile{};
  entry.draft_present = false;
  entry.state = ModelLifecycleState::Active;
  return true;
}

bool ModelProfileLifecycle::cancel(ModelProfileKey key) noexcept {
  const auto entry_index = index(key);
  if (entry_index >= entries_.size()) return false;
  auto& entry = entries_[entry_index];
  entry.draft = EncodedModelProfile{};
  entry.draft_present = false;
  entry.state = entry.active_present ? ModelLifecycleState::Active
                                     : ModelLifecycleState::Unloaded;
  return true;
}

bool ModelProfileLifecycle::rollback(ModelProfileKey key) noexcept {
  if (persistence_.rollback(key) != ModelLoadStatus::Found) {
    return false;
  }
  return load(key);
}

bool ModelProfileLifecycle::snapshot(
    ModelProfileKey key, ModelProfileSnapshot& output) const noexcept {
  const auto entry_index = index(key);
  if (entry_index >= entries_.size()) return false;
  const auto& entry = entries_[entry_index];
  output = ModelProfileSnapshot{};
  if (!entry.active_present) return true;
  output.present = true;
  output.ref = to_ref(entry.active_identity);
  output.encoded = entry.active;
  return true;
}

ModelLifecycleState ModelProfileLifecycle::state(
    ModelProfileKey key) const noexcept {
  const auto entry_index = index(key);
  return entry_index < entries_.size() ? entries_[entry_index].state
                                       : ModelLifecycleState::Error;
}

}  // namespace sigap::models
