#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sigap::maintenance {

constexpr std::size_t kMaxCommandLineBytes{255U};
constexpr std::size_t kMaxBytesPerPoll{32U};
constexpr std::size_t kMaxResponseBytes{192U};

template <std::size_t Capacity>
struct FixedText final {
  std::array<char, Capacity> bytes{};
  std::uint16_t size{0U};

  bool assign(std::string_view value) noexcept {
    if (value.size() >= Capacity) return false;
    for (std::size_t i = 0U; i < value.size(); ++i) bytes[i] = value[i];
    bytes[value.size()] = '\0';
    size = static_cast<std::uint16_t>(value.size());
    return true;
  }

  [[nodiscard]] const char* c_str() const noexcept { return bytes.data(); }
  [[nodiscard]] std::string_view view() const noexcept {
    return std::string_view(bytes.data(), size);
  }
  void clear() noexcept {
    volatile char* output = bytes.data();
    for (std::size_t i = 0U; i < bytes.size(); ++i) output[i] = '\0';
    size = 0U;
  }
};

enum class CommandKind : std::uint8_t {
  Unknown,
  Status,
  MaintenanceEnter,
  MaintenanceExit,
  ConfigActive,
  ConfigDraftStart,
  ConfigSet,
  ConfigValidate,
  ConfigPreview,
  ConfigCommit,
  ConfigCancel,
  ConfigReset,
  SecretStatus,
  SecretSetWifi,
  SecretRotateToken,
  SecretClear,
  CalibrationStart,
  CalibrationCriteria,
  CalibrationStatus,
  CalibrationCapture,
  CalibrationValidate,
  CalibrationPreview,
  CalibrationCommit,
  CalibrationCancel,
  ProfileStatus,
  ProfileExport,
  ProfileImport,
  ProfileImportBegin,
  ProfileImportChunk,
  ProfileImportFinish,
  ProfileValidate,
  ProfilePreview,
  ProfileCommit,
  ProfileRollback,
  ProfileReset,
  ModelStatus,
  ModelExport,
  ModelImportBegin,
  ModelImportChunk,
  ModelImportFinish,
  ModelValidate,
  ModelPreview,
  ModelCommit,
  ModelCancel,
  ModelRollback,
  Reboot,
};

enum class CommandStatus : std::uint8_t {
  Accepted,
  Empty,
  UnknownCommand,
  InvalidSyntax,
  InvalidNumeric,
  Oversized,
  Rejected,
  Failed,
};

[[nodiscard]] const char* command_name(CommandKind command) noexcept;
[[nodiscard]] const char* status_name(CommandStatus status) noexcept;

struct MaintenanceCommand final {
  CommandKind kind{CommandKind::Unknown};
  FixedText<64U> target{};
  FixedText<64U> field{};
  FixedText<129U> value{};
  std::int64_t numeric_value{0};
  bool has_numeric{false};

  void clear_sensitive() noexcept {
    target.clear();
    field.clear();
    value.clear();
    numeric_value = 0;
    has_numeric = false;
  }
};

struct ParseEvent final {
  CommandStatus status{CommandStatus::Empty};
  MaintenanceCommand command{};
};

class MaintenanceParser final {
 public:
  MaintenanceParser() noexcept = default;

  // Discard a partial line on transport loss. This also zeroizes sensitive
  // bytes that may have been buffered before a reconnect.
  void reset() noexcept;

  // Emits one event only when a complete line or rejected oversized line is
  // received. Input may be fragmented across calls.
  bool consume(char byte, ParseEvent& event) noexcept;

 private:
  bool emit_line(ParseEvent& event) noexcept;

  std::array<char, kMaxCommandLineBytes + 1U> line_{};
  std::size_t size_{0U};
  bool oversized_{false};
  bool swallow_lf_{false};
};

class MaintenanceResponse final {
 public:
  bool append(std::string_view value) noexcept;
  bool append_status(CommandStatus status) noexcept;

  // The secret argument is deliberately ignored. This primitive makes it
  // difficult for maintenance handlers to accidentally echo credentials.
  bool append_secret(std::string_view label,
                     std::string_view secret) noexcept;
  bool append_presence(std::string_view label, bool present) noexcept;

  [[nodiscard]] std::string_view view() const noexcept {
    return std::string_view(bytes_.data(), size_);
  }
  [[nodiscard]] const char* c_str() const noexcept { return bytes_.data(); }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }

 private:
  std::array<char, kMaxResponseBytes + 1U> bytes_{};
  std::size_t size_{0U};
};

class IMaintenanceTransport {
 public:
  virtual ~IMaintenanceTransport() = default;
  virtual bool available() noexcept = 0;
  virtual int read() noexcept = 0;
  virtual bool write(std::string_view value) noexcept = 0;
  virtual bool connected() noexcept { return true; }
};

class IMaintenanceCommandSink {
 public:
  virtual ~IMaintenanceCommandSink() = default;
  virtual CommandStatus execute(const MaintenanceCommand& command,
                                std::uint64_t now_us,
                                MaintenanceResponse& response) noexcept = 0;
  virtual void poll(std::uint64_t) noexcept {}
  virtual void transport_lost() noexcept {}
};

class IModelCommandHandler {
 public:
  virtual ~IModelCommandHandler() = default;
  virtual CommandStatus execute_model(const MaintenanceCommand& command,
                                      std::uint64_t now_us,
                                      MaintenanceResponse& response) noexcept = 0;
  virtual void poll_model(std::uint64_t now_us) noexcept = 0;
  virtual void cancel_model() noexcept = 0;
};

struct MaintenancePollStats final {
  std::size_t bytes_read{0U};
  std::size_t commands_dispatched{0U};
  std::size_t parser_errors{0U};
  bool transport_write_failed{false};
};

class MaintenanceService final {
 public:
  MaintenanceService(IMaintenanceTransport& transport,
                     IMaintenanceCommandSink& sink) noexcept
      : transport_(transport), sink_(sink) {}

  MaintenancePollStats poll(std::uint64_t now_us) noexcept;

  [[nodiscard]] std::size_t commands_dispatched() const noexcept {
    return commands_dispatched_;
  }

 private:
  void dispatch(ParseEvent& event, std::uint64_t now_us,
                MaintenancePollStats& stats) noexcept;

  IMaintenanceTransport& transport_;
  IMaintenanceCommandSink& sink_;
  MaintenanceParser parser_{};
  std::size_t commands_dispatched_{0U};
  bool connected_last_{true};
};

}  // namespace sigap::maintenance
