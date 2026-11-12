#pragma once

#include <cstddef>
#include <cstdint>

#include "maintenance/maintenance.h"
#include "models/model_lifecycle.h"
#include "debug/io_gate.h"

namespace sigap::maintenance {

// Model transfer is deliberately separate from calibration profile transfer:
// model envelopes are larger and have a distinct typed key space.
class ModelMaintenanceRouter final : public IModelCommandHandler {
 public:
  static constexpr uint64_t kTransferTimeoutUs{300000000ULL};

  explicit ModelMaintenanceRouter(models::ModelProfileLifecycle& lifecycle,
                                  const models::ModelProfileCodec& codec) noexcept
      : lifecycle_(lifecycle), codec_(codec) {}

  CommandStatus execute_model(const MaintenanceCommand& command,
                              uint64_t now_us,
                              MaintenanceResponse& response) noexcept override;
  void poll_model(uint64_t now_us) noexcept override;
  void cancel_model() noexcept override;
  // Optional app-owned admission binding; unbound preserves legacy behavior.
  void set_io_gate(debug::RuntimeIoGate* gate) noexcept { io_gate_ = gate; }

 private:
  static bool key_from_text(const FixedText<64U>& text,
                            models::ModelProfileKey& key) noexcept;
  static bool decode_hex(std::string_view text, uint8_t* output,
                        std::size_t capacity, std::size_t& written) noexcept;
  static bool decode_hash(std::string_view text,
                          config::ConfigHash& output) noexcept;
  static bool append_hex(MaintenanceResponse& response, const uint8_t* bytes,
                         std::size_t size) noexcept;
  void cancel_transfer() noexcept;
  void clear_transfer_state() noexcept;
  bool command_targets_transfer(const MaintenanceCommand& command) const noexcept;

  models::ModelProfileLifecycle& lifecycle_;
  const models::ModelProfileCodec& codec_;
  debug::RuntimeIoGate* io_gate_{nullptr};
  models::ModelProfileKey transfer_key_{};
  models::EncodedModelProfile transfer_profile_{};
  config::ConfigHash expected_hash_{};
  std::size_t expected_size_{0U};
  std::size_t received_size_{0U};
  std::size_t export_offset_{0U};
  uint64_t last_activity_us_{0U};
  bool import_active_{false};
  bool export_active_{false};
  bool draft_active_{false};
  // A draft is owned only after this router successfully stages an import.
  // Existing lifecycle drafts are never cancelled as a side effect of an
  // unrelated transfer.
  bool draft_owned_{false};
  models::ModelProfileKey draft_key_{};
};

}  // namespace sigap::maintenance
