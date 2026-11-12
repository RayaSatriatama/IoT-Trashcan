#include "maintenance/model_runtime.h"

#include <algorithm>
#include <charconv>

namespace sigap::maintenance {
namespace {

bool text_equals(std::string_view value, const char* expected) noexcept {
  std::size_t length{0U};
  while (expected[length] != '\0') ++length;
  return value == std::string_view(expected, length);
}

int hex_digit(char value) noexcept {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

}  // namespace

bool ModelMaintenanceRouter::key_from_text(
    const FixedText<64U>& text, models::ModelProfileKey& key) noexcept {
  const auto value = text.view();
  if (text_equals(value, "fis_organic")) {
    key = {acquisition::ProfileKind::Fis, models::ModelTarget::FisOrganic};
    return true;
  }
  if (text_equals(value, "fis_inorganic")) {
    key = {acquisition::ProfileKind::Fis, models::ModelTarget::FisInorganic};
    return true;
  }
  if (text_equals(value, "fis_b3")) {
    key = {acquisition::ProfileKind::Fis, models::ModelTarget::FisB3};
    return true;
  }
  if (text_equals(value, "safety_b3")) {
    key = {acquisition::ProfileKind::Safety, models::ModelTarget::SafetyB3};
    return true;
  }
  return false;
}

bool ModelMaintenanceRouter::decode_hex(std::string_view text, uint8_t* output,
                                        std::size_t capacity,
                                        std::size_t& written) noexcept {
  if (output == nullptr || (text.size() % 2U) != 0U ||
      text.size() / 2U > capacity) return false;
  written = text.size() / 2U;
  for (std::size_t index = 0U; index < written; ++index) {
    const int high = hex_digit(text[index * 2U]);
    const int low = hex_digit(text[index * 2U + 1U]);
    if (high < 0 || low < 0) return false;
    output[index] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

bool ModelMaintenanceRouter::decode_hash(
    std::string_view text, config::ConfigHash& output) noexcept {
  if (text.substr(0U, 5U) == "hash=") text.remove_prefix(5U);
  std::size_t written{0U};
  return text.size() == output.size() * 2U &&
         decode_hex(text, output.data(), output.size(), written) &&
         written == output.size();
}

bool ModelMaintenanceRouter::append_hex(MaintenanceResponse& response,
                                        const uint8_t* bytes,
                                        std::size_t size) noexcept {
  static constexpr char digits[] = "0123456789abcdef";
  if (bytes == nullptr) return false;
  for (std::size_t index = 0U; index < size; ++index) {
    char pair[2]{digits[bytes[index] >> 4U], digits[bytes[index] & 0x0fU]};
    if (!response.append(std::string_view(pair, 2U))) return false;
  }
  return true;
}

bool append_size(MaintenanceResponse& response, std::size_t value) noexcept {
  char digits[24]{};
  const auto converted = std::to_chars(digits, digits + sizeof(digits), value);
  return converted.ec == std::errc{} &&
         response.append(std::string_view(digits,
                                          static_cast<std::size_t>(converted.ptr - digits)));
}

void ModelMaintenanceRouter::clear_transfer_state() noexcept {
  import_active_ = false;
  export_active_ = false;
  expected_size_ = 0U;
  received_size_ = 0U;
  export_offset_ = 0U;
  last_activity_us_ = 0U;
  expected_hash_ = config::ConfigHash{};
  transfer_profile_ = models::EncodedModelProfile{};
}

void ModelMaintenanceRouter::cancel_transfer() noexcept {
  if (draft_owned_) (void)lifecycle_.cancel(draft_key_);
  draft_owned_ = false;
  draft_active_ = false;
  draft_key_ = models::ModelProfileKey{};
  transfer_key_ = models::ModelProfileKey{};
  clear_transfer_state();
}

bool ModelMaintenanceRouter::command_targets_transfer(
    const MaintenanceCommand& command) const noexcept {
  if (command.target.view().empty()) return true;
  models::ModelProfileKey key{};
  return key_from_text(command.target, key) && key == transfer_key_;
}

void ModelMaintenanceRouter::cancel_model() noexcept { cancel_transfer(); }

void ModelMaintenanceRouter::poll_model(uint64_t now_us) noexcept {
  if ((import_active_ || export_active_ || draft_active_) &&
      now_us >= last_activity_us_ &&
      now_us - last_activity_us_ > kTransferTimeoutUs) {
    cancel_transfer();
  }
}

CommandStatus ModelMaintenanceRouter::execute_model(
    const MaintenanceCommand& command, uint64_t now_us,
    MaintenanceResponse& response) noexcept {
  if (io_gate_ != nullptr && io_gate_->active(debug::IoOwner::DebugHttp) &&
      command.kind != CommandKind::ModelStatus &&
      command.kind != CommandKind::ModelCancel) {
    (void)response.append("ERR STORAGE_BUSY\n");
    return CommandStatus::Rejected;
  }
  models::ModelProfileKey key{};
  if (command.kind == CommandKind::ModelImportChunk) {
    if (!import_active_) return CommandStatus::Rejected;
    if (!command_targets_transfer(command)) {
      cancel_transfer();
      return CommandStatus::Rejected;
    }
    key = transfer_key_;
  } else if (!key_from_text(command.target, key)) {
    return CommandStatus::InvalidSyntax;
  }

  // A transfer has one owner and one bound key.  Competing operations are
  // rejected without changing the in-flight bytes or its timeout clock.
  if (import_active_ && command.kind != CommandKind::ModelImportChunk &&
      command.kind != CommandKind::ModelImportFinish &&
      command.kind != CommandKind::ModelCancel &&
      command.kind != CommandKind::ModelStatus) {
    return CommandStatus::Rejected;
  }
  if (export_active_ && command.kind != CommandKind::ModelExport &&
      command.kind != CommandKind::ModelCancel &&
      command.kind != CommandKind::ModelStatus) {
    return CommandStatus::Rejected;
  }
  if (draft_owned_ && (command.kind == CommandKind::ModelImportBegin ||
                       command.kind == CommandKind::ModelExport)) {
    return CommandStatus::Rejected;
  }
  if ((import_active_ || export_active_) &&
      (command.kind == CommandKind::ModelCancel ||
       command.kind == CommandKind::ModelStatus) &&
      !(key == transfer_key_)) {
    return CommandStatus::Rejected;
  }

  // Only accepted transfer activity refreshes the timeout.
  if (command.kind != CommandKind::ModelImportBegin &&
      command.kind != CommandKind::ModelStatus &&
      command.kind != CommandKind::ModelValidate &&
      command.kind != CommandKind::ModelPreview &&
      command.kind != CommandKind::ModelCommit &&
      command.kind != CommandKind::ModelCancel &&
      command.kind != CommandKind::ModelRollback) {
    last_activity_us_ = now_us;
  }

  if (command.kind == CommandKind::ModelExport) {
    if (export_active_ && !(key == transfer_key_)) return CommandStatus::Rejected;
    if (!export_active_) {
      models::ModelProfileSnapshot snapshot{};
      if (!lifecycle_.snapshot(key, snapshot) || !snapshot.present) {
        return CommandStatus::Rejected;
      }
      transfer_key_ = key;
      transfer_profile_ = snapshot.encoded;
      export_offset_ = 0U;
      export_active_ = true;
    }
    if (export_offset_ >= transfer_profile_.size) {
      cancel_transfer();
      return CommandStatus::Rejected;
    }
    if (export_offset_ == 0U) {
      if (!response.append("MODEL BEGIN size=") ||
          !append_size(response, transfer_profile_.size) ||
          !response.append(" hash=") ||
          !append_hex(response, transfer_profile_.hash().data(),
                      transfer_profile_.hash().size()) ||
          !response.append("\nDATA ")) {
        return CommandStatus::Failed;
      }
    } else if (!response.append("DATA ")) {
      return CommandStatus::Failed;
    }
    const std::size_t remaining = transfer_profile_.size - export_offset_;
    const std::size_t capacity = (kMaxResponseBytes - response.size() - 2U) / 2U;
    const std::size_t chunk = std::min(remaining, capacity);
    if (chunk == 0U ||
        !append_hex(response, transfer_profile_.bytes.data() + export_offset_,
                    chunk) ||
        !response.append("\n")) return CommandStatus::Failed;
    export_offset_ += chunk;
    if (export_offset_ == transfer_profile_.size) clear_transfer_state();
    return CommandStatus::Accepted;
  }

  if (command.kind == CommandKind::ModelImportBegin) {
    if (import_active_ || export_active_ || draft_owned_) {
      return CommandStatus::Rejected;
    }
    config::ConfigHash requested_hash{};
    if (!command.has_numeric || command.numeric_value <= 0 ||
        static_cast<std::size_t>(command.numeric_value) >
            transfer_profile_.bytes.size() ||
        !decode_hash(command.value.view(), requested_hash)) {
      return CommandStatus::InvalidSyntax;
    }
    transfer_key_ = key;
    transfer_profile_ = models::EncodedModelProfile{};
    expected_hash_ = requested_hash;
    expected_size_ = static_cast<std::size_t>(command.numeric_value);
    received_size_ = 0U;
    export_offset_ = 0U;
    import_active_ = true;
    last_activity_us_ = now_us;
    return CommandStatus::Accepted;
  }

  if (command.kind == CommandKind::ModelImportChunk) {
    if (!import_active_ || (command.has_numeric &&
        command.numeric_value != static_cast<int64_t>(received_size_))) {
      cancel_transfer();
      return CommandStatus::InvalidSyntax;
    }
    std::array<uint8_t, models::kMaxEncodedModelBytes> chunk_bytes{};
    std::size_t written{0U};
    if (!decode_hex(command.value.view(), chunk_bytes.data(), chunk_bytes.size(),
                    written) || written == 0U ||
        received_size_ + written > expected_size_) {
      cancel_transfer();
      return CommandStatus::InvalidSyntax;
    }
    std::copy_n(chunk_bytes.data(), written,
                transfer_profile_.bytes.begin() + received_size_);
    received_size_ += written;
    last_activity_us_ = now_us;
    return CommandStatus::Accepted;
  }

  if (command.kind == CommandKind::ModelImportFinish) {
    if (!import_active_ || !command_targets_transfer(command) ||
        received_size_ != expected_size_) {
      cancel_transfer();
      return CommandStatus::Rejected;
    }
    transfer_profile_.size = received_size_;
    const bool hash_ok = transfer_profile_.hash() == expected_hash_;
    const bool codec_ok = codec_.validate(transfer_profile_, key);
    // Validate the complete candidate before touching lifecycle draft state.
    if (!hash_ok || !codec_ok) {
      cancel_transfer();
      return CommandStatus::Rejected;
    }
    if (!lifecycle_.stage(key, transfer_profile_)) {
      cancel_transfer();
      return CommandStatus::Rejected;
    }
    import_active_ = false;
    draft_active_ = true;
    draft_owned_ = true;
    draft_key_ = key;
    expected_size_ = 0U;
    received_size_ = 0U;
    export_offset_ = 0U;
    expected_hash_ = config::ConfigHash{};
    transfer_profile_ = models::EncodedModelProfile{};
    last_activity_us_ = now_us;
    return CommandStatus::Accepted;
  }

  if (command.kind == CommandKind::ModelStatus) {
    const auto state = lifecycle_.state(key);
    if (!response.append("model_state=") ||
        !response.append(state == models::ModelLifecycleState::Active ? "ACTIVE" :
                         state == models::ModelLifecycleState::Draft ? "DRAFT" :
                         state == models::ModelLifecycleState::Validated ? "VALIDATED" :
                         state == models::ModelLifecycleState::Preview ? "PREVIEW" :
                         state == models::ModelLifecycleState::Error ? "ERROR" : "UNLOADED") ||
        !response.append("\n")) return CommandStatus::Failed;
    return CommandStatus::Accepted;
  }

  if (command.kind == CommandKind::ModelValidate) {
    if (draft_owned_ && !(key == draft_key_)) return CommandStatus::Rejected;
    return lifecycle_.validate(key) ? CommandStatus::Accepted : CommandStatus::Rejected;
  }
  if (command.kind == CommandKind::ModelPreview) {
    if (draft_owned_ && !(key == draft_key_)) return CommandStatus::Rejected;
    return lifecycle_.preview(key) ? CommandStatus::Accepted : CommandStatus::Rejected;
  }
  if (command.kind == CommandKind::ModelCommit) {
    if (draft_owned_ && !(key == draft_key_)) return CommandStatus::Rejected;
    const bool ok = lifecycle_.commit(key);
    if (ok && draft_owned_ && key == draft_key_) {
      draft_active_ = false;
      draft_owned_ = false;
      draft_key_ = models::ModelProfileKey{};
    }
    return ok ? CommandStatus::Accepted : CommandStatus::Rejected;
  }
  if (command.kind == CommandKind::ModelCancel) {
    if (draft_owned_ && !(key == draft_key_)) return CommandStatus::Rejected;
    if (import_active_ || export_active_) {
      cancel_transfer();
      return CommandStatus::Accepted;
    }
    const bool ok = lifecycle_.cancel(key);
    if (ok) cancel_transfer();
    return ok ? CommandStatus::Accepted : CommandStatus::Rejected;
  }
  if (command.kind == CommandKind::ModelRollback) {
    if (draft_owned_ && !(key == draft_key_)) return CommandStatus::Rejected;
    return lifecycle_.rollback(key) ? CommandStatus::Accepted : CommandStatus::Rejected;
  }
  return CommandStatus::Rejected;
}

}  // namespace sigap::maintenance
