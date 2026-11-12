#pragma once
#include "debug/service.h"
#include "maintenance/maintenance.h"
namespace sigap::debug {
class CommandBackend final : public maintenance::IDebugCommandHandler {
 public:
  explicit CommandBackend(DebugService& service) noexcept : service_(service) {}
  bool meta(uint64_t now_us, char* output, size_t capacity, size_t& length) noexcept;
  bool page(const Cursor& cursor, char* output, size_t capacity, size_t& length) noexcept;
  bool set_level(Level level, char* output, size_t capacity, size_t& length) noexcept;
  maintenance::CommandStatus execute_debug(
      const maintenance::MaintenanceCommand&, uint64_t,
      maintenance::MaintenanceResponse&) noexcept override;
 private:
  DebugService& service_;
};
}
