#include "maintenance/maintenance.h"

#include <charconv>
#include <cstring>

namespace sigap::maintenance {
namespace {

struct Token final {
  std::size_t offset{0U};
  std::size_t length{0U};
};

constexpr std::size_t kMaxTokens{6U};

bool equals(const char* line, Token token, const char* value) noexcept {
  const auto length = std::strlen(value);
  return token.length == length &&
         std::memcmp(line + token.offset, value, length) == 0;
}

template <std::size_t Capacity>
bool copy_token(const char* line, Token token,
                FixedText<Capacity>& output) noexcept {
  return output.assign(std::string_view(line + token.offset, token.length));
}

bool parse_integer(const char* line, Token token, std::int64_t& output) noexcept {
  const char* first = line + token.offset;
  const char* last = first + token.length;
  const auto parsed = std::from_chars(first, last, output);
  return parsed.ec == std::errc{} && parsed.ptr == last;
}

bool numeric_field(const FixedText<64U>& field) noexcept {
  const std::string_view value = field.view();
  const auto ends_with = [value](std::string_view suffix) noexcept {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
  };
  return ends_with("_ms") || ends_with("_bytes") ||
         value == "max_records_per_batch";
}

bool parse_tokens(const char* line, std::size_t length,
                  std::array<Token, kMaxTokens>& tokens,
                  std::size_t& count) noexcept {
  count = 0U;
  std::size_t offset = 0U;
  while (offset < length) {
    while (offset < length &&
           (line[offset] == ' ' || line[offset] == '\t')) {
      ++offset;
    }
    if (offset == length) break;
    if (count == tokens.size()) return false;
    const std::size_t start = offset;
    while (offset < length && line[offset] != ' ' && line[offset] != '\t') {
      ++offset;
    }
    tokens[count++] = Token{start, offset - start};
  }
  return true;
}

}  // namespace

const char* command_name(CommandKind command) noexcept {
  switch (command) {
    case CommandKind::Status: return "status";
    case CommandKind::MaintenanceEnter: return "maintenance.enter";
    case CommandKind::MaintenanceExit: return "maintenance.exit";
    case CommandKind::ConfigActive: return "config.active";
    case CommandKind::ConfigDraftStart: return "config.draft.start";
    case CommandKind::ConfigSet: return "config.set";
    case CommandKind::ConfigValidate: return "config.validate";
    case CommandKind::ConfigPreview: return "config.preview";
    case CommandKind::ConfigCommit: return "config.commit";
    case CommandKind::ConfigCancel: return "config.cancel";
    case CommandKind::ConfigReset: return "config.reset";
    case CommandKind::SecretStatus: return "secret.status";
    case CommandKind::SecretSetWifi: return "secret.set_wifi";
    case CommandKind::SecretRotateToken: return "secret.rotate_token";
    case CommandKind::SecretClear: return "secret.clear";
    case CommandKind::CalibrationStart: return "calibration.start";
    case CommandKind::CalibrationCriteria: return "calibration.criteria";
    case CommandKind::CalibrationStatus: return "calibration.status";
    case CommandKind::CalibrationCapture: return "calibration.capture";
    case CommandKind::CalibrationValidate: return "calibration.validate";
    case CommandKind::CalibrationPreview: return "calibration.preview";
    case CommandKind::CalibrationCommit: return "calibration.commit";
    case CommandKind::CalibrationCancel: return "calibration.cancel";
    case CommandKind::ProfileStatus: return "profile.status";
    case CommandKind::ProfileExport: return "profile.export";
    case CommandKind::ProfileImport: return "profile.import";
    case CommandKind::ProfileImportBegin: return "profile.import.begin";
    case CommandKind::ProfileImportChunk: return "profile.import.chunk";
    case CommandKind::ProfileImportFinish: return "profile.import.finish";
    case CommandKind::ProfileValidate: return "profile.validate";
    case CommandKind::ProfilePreview: return "profile.preview";
    case CommandKind::ProfileCommit: return "profile.commit";
    case CommandKind::ProfileRollback: return "profile.rollback";
    case CommandKind::ProfileReset: return "profile.reset";
    case CommandKind::ModelStatus: return "model.status";
    case CommandKind::ModelExport: return "model.export";
    case CommandKind::ModelImportBegin: return "model.import.begin";
    case CommandKind::ModelImportChunk: return "model.import.chunk";
    case CommandKind::ModelImportFinish: return "model.import.finish";
    case CommandKind::ModelValidate: return "model.validate";
    case CommandKind::ModelPreview: return "model.preview";
    case CommandKind::ModelCommit: return "model.commit";
    case CommandKind::ModelCancel: return "model.cancel";
    case CommandKind::ModelRollback: return "model.rollback";
    case CommandKind::StorageSdTest: return "storage.sd.test";
    case CommandKind::Reboot: return "reboot";
    case CommandKind::Unknown: break;
  }
  return "unknown";
}

const char* status_name(CommandStatus status) noexcept {
  switch (status) {
    case CommandStatus::Accepted: return "OK";
    case CommandStatus::Empty: return "EMPTY";
    case CommandStatus::UnknownCommand: return "UNKNOWN_COMMAND";
    case CommandStatus::InvalidSyntax: return "INVALID_SYNTAX";
    case CommandStatus::InvalidNumeric: return "INVALID_NUMERIC";
    case CommandStatus::Oversized: return "OVERSIZED";
    case CommandStatus::Rejected: return "REJECTED";
    case CommandStatus::Failed: return "FAILED";
  }
  return "FAILED";
}

bool MaintenanceParser::consume(char byte, ParseEvent& event) noexcept {
  event = ParseEvent{};
  if (swallow_lf_ && byte == '\n') {
    swallow_lf_ = false;
    return false;
  }
  swallow_lf_ = false;
  if (byte == '\n' || byte == '\r') {
    if (byte == '\r') swallow_lf_ = true;
    return emit_line(event);
  }
  if (oversized_) return false;
  if (size_ >= kMaxCommandLineBytes) {
    oversized_ = true;
    return false;
  }
  line_[size_++] = byte;
  return false;
}

void MaintenanceParser::reset() noexcept {
  volatile char* bytes = line_.data();
  for (std::size_t index = 0U; index < line_.size(); ++index) {
    bytes[index] = '\0';
  }
  size_ = 0U;
  oversized_ = false;
}

bool MaintenanceParser::emit_line(ParseEvent& event) noexcept {
  if (oversized_) {
    event.status = CommandStatus::Oversized;
    reset();
    return true;
  }
  line_[size_] = '\0';
  if (size_ == 0U) {
    event.status = CommandStatus::Empty;
    reset();
    return true;
  }

  std::array<Token, kMaxTokens> tokens{};
  std::size_t count = 0U;
  if (!parse_tokens(line_.data(), size_, tokens, count)) {
    event.status = CommandStatus::InvalidSyntax;
    reset();
    return true;
  }
  if (count == 0U) {
    event.status = CommandStatus::Empty;
    reset();
    return true;
  }

  MaintenanceCommand command{};
  bool valid = true;
  bool numeric = false;
  Token numeric_token{};

  if (equals(line_.data(), tokens[0], "status") && count == 1U) {
    command.kind = CommandKind::Status;
  } else if (equals(line_.data(), tokens[0], "reboot") && count == 1U) {
    command.kind = CommandKind::Reboot;
  } else if (equals(line_.data(), tokens[0], "maintenance") && count == 2U) {
    if (equals(line_.data(), tokens[1], "enter"))
      command.kind = CommandKind::MaintenanceEnter;
    else if (equals(line_.data(), tokens[1], "exit"))
      command.kind = CommandKind::MaintenanceExit;
    else
      valid = false;
  } else if (equals(line_.data(), tokens[0], "config")) {
    if (count == 2U && equals(line_.data(), tokens[1], "active"))
      command.kind = CommandKind::ConfigActive;
    else if (count == 2U && equals(line_.data(), tokens[1], "validate"))
      command.kind = CommandKind::ConfigValidate;
    else if (count == 2U && equals(line_.data(), tokens[1], "preview"))
      command.kind = CommandKind::ConfigPreview;
    else if (count == 2U && equals(line_.data(), tokens[1], "commit"))
      command.kind = CommandKind::ConfigCommit;
    else if (count == 2U && equals(line_.data(), tokens[1], "cancel"))
      command.kind = CommandKind::ConfigCancel;
    else if (count == 2U && equals(line_.data(), tokens[1], "reset"))
      command.kind = CommandKind::ConfigReset;
    else if (count == 2U && equals(line_.data(), tokens[1], "draft"))
      valid = false;
    else if (count == 3U && equals(line_.data(), tokens[1], "draft") &&
             equals(line_.data(), tokens[2], "start"))
      command.kind = CommandKind::ConfigDraftStart;
    else if (count == 4U && equals(line_.data(), tokens[1], "set")) {
      command.kind = CommandKind::ConfigSet;
      valid = copy_token(line_.data(), tokens[2], command.field) &&
              copy_token(line_.data(), tokens[3], command.value);
      numeric = numeric_field(command.field);
      numeric_token = tokens[3];
    } else {
      valid = false;
    }
  } else if (equals(line_.data(), tokens[0], "model")) {
    if (count == 3U && equals(line_.data(), tokens[1], "status")) {
      command.kind = CommandKind::ModelStatus;
      valid = copy_token(line_.data(), tokens[2], command.target);
    } else if (count == 3U && equals(line_.data(), tokens[1], "export")) {
      command.kind = CommandKind::ModelExport;
      valid = copy_token(line_.data(), tokens[2], command.target);
    } else if (count == 6U && equals(line_.data(), tokens[1], "import") &&
               equals(line_.data(), tokens[2], "begin")) {
      command.kind = CommandKind::ModelImportBegin;
      valid = copy_token(line_.data(), tokens[3], command.target) &&
              copy_token(line_.data(), tokens[5], command.value);
      numeric = true;
      numeric_token = tokens[4];
    } else if (count == 4U && equals(line_.data(), tokens[1], "import") &&
               equals(line_.data(), tokens[2], "chunk")) {
      command.kind = CommandKind::ModelImportChunk;
      valid = copy_token(line_.data(), tokens[3], command.value);
    } else if (count == 5U && equals(line_.data(), tokens[1], "import") &&
               equals(line_.data(), tokens[2], "chunk")) {
      command.kind = CommandKind::ModelImportChunk;
      valid = copy_token(line_.data(), tokens[4], command.value);
      numeric = true;
      numeric_token = tokens[3];
    } else if (count == 4U && equals(line_.data(), tokens[1], "import") &&
               equals(line_.data(), tokens[2], "finish")) {
      command.kind = CommandKind::ModelImportFinish;
      valid = copy_token(line_.data(), tokens[3], command.target);
    } else if (count == 3U && equals(line_.data(), tokens[1], "validate")) {
      command.kind = CommandKind::ModelValidate;
      valid = copy_token(line_.data(), tokens[2], command.target);
    } else if (count == 3U && equals(line_.data(), tokens[1], "preview")) {
      command.kind = CommandKind::ModelPreview;
      valid = copy_token(line_.data(), tokens[2], command.target);
    } else if (count == 3U && equals(line_.data(), tokens[1], "commit")) {
      command.kind = CommandKind::ModelCommit;
      valid = copy_token(line_.data(), tokens[2], command.target);
    } else if (count == 3U && equals(line_.data(), tokens[1], "cancel")) {
      command.kind = CommandKind::ModelCancel;
      valid = copy_token(line_.data(), tokens[2], command.target);
    } else if (count == 3U && equals(line_.data(), tokens[1], "rollback")) {
      command.kind = CommandKind::ModelRollback;
      valid = copy_token(line_.data(), tokens[2], command.target);
    } else {
      valid = false;
    }
  } else if (equals(line_.data(), tokens[0], "secret")) {
    if (count == 2U && equals(line_.data(), tokens[1], "status"))
      command.kind = CommandKind::SecretStatus;
    else if (count == 5U && equals(line_.data(), tokens[1], "set") &&
             equals(line_.data(), tokens[2], "wifi")) {
      command.kind = CommandKind::SecretSetWifi;
      valid = copy_token(line_.data(), tokens[3], command.target) &&
              copy_token(line_.data(), tokens[4], command.value);
    } else if (count == 4U && equals(line_.data(), tokens[1], "rotate") &&
               equals(line_.data(), tokens[2], "token")) {
      command.kind = CommandKind::SecretRotateToken;
      valid = copy_token(line_.data(), tokens[3], command.value);
    } else if (count == 3U && equals(line_.data(), tokens[1], "clear")) {
      command.kind = CommandKind::SecretClear;
      valid = copy_token(line_.data(), tokens[2], command.value) &&
              command.value.view() == "CONFIRM";
    } else {
      valid = false;
    }
  } else if (equals(line_.data(), tokens[0], "storage")) {
    if (count == 3U && equals(line_.data(), tokens[1], "sd") &&
        equals(line_.data(), tokens[2], "test")) {
      command.kind = CommandKind::StorageSdTest;
    } else {
      valid = false;
    }
  } else if (equals(line_.data(), tokens[0], "calibration")) {
    if (count == 2U && equals(line_.data(), tokens[1], "status"))
      command.kind = CommandKind::CalibrationStatus;
    else if (count == 3U && equals(line_.data(), tokens[1], "start")) {
      command.kind = CommandKind::CalibrationStart;
      valid = copy_token(line_.data(), tokens[2], command.target);
    } else if (count == 4U && equals(line_.data(), tokens[1], "criteria")) {
      command.kind = CommandKind::CalibrationCriteria;
      valid = copy_token(line_.data(), tokens[2], command.field);
      numeric = true;
      numeric_token = tokens[3];
    } else if (count == 3U && equals(line_.data(), tokens[1], "capture")) {
      command.kind = CommandKind::CalibrationCapture;
      valid = copy_token(line_.data(), tokens[2], command.target);
    } else if (count == 4U && equals(line_.data(), tokens[1], "capture")) {
      command.kind = CommandKind::CalibrationCapture;
      valid = copy_token(line_.data(), tokens[2], command.target);
      numeric = true;
      numeric_token = tokens[3];
    } else if (count == 2U && equals(line_.data(), tokens[1], "validate"))
      command.kind = CommandKind::CalibrationValidate;
    else if (count == 2U && equals(line_.data(), tokens[1], "preview"))
      command.kind = CommandKind::CalibrationPreview;
    else if (count == 2U && equals(line_.data(), tokens[1], "commit"))
      command.kind = CommandKind::CalibrationCommit;
    else if (count == 2U && equals(line_.data(), tokens[1], "cancel"))
      command.kind = CommandKind::CalibrationCancel;
    else
      valid = false;
  } else if (equals(line_.data(), tokens[0], "profile")) {
    if (count == 3U && equals(line_.data(), tokens[1], "status")) {
      command.kind = CommandKind::ProfileStatus;
      valid = copy_token(line_.data(), tokens[2], command.target);
    } else if (count == 3U && equals(line_.data(), tokens[1], "export")) {
      command.kind = CommandKind::ProfileExport;
      valid = copy_token(line_.data(), tokens[2], command.target);
    } else if (count == 3U && equals(line_.data(), tokens[1], "import")) {
      command.kind = CommandKind::ProfileImport;
      valid = copy_token(line_.data(), tokens[2], command.target);
    } else if (count == 6U && equals(line_.data(), tokens[1], "import") &&
               equals(line_.data(), tokens[2], "begin")) {
      command.kind = CommandKind::ProfileImportBegin;
      valid = copy_token(line_.data(), tokens[3], command.target) &&
              copy_token(line_.data(), tokens[5], command.value);
      numeric = true;
      numeric_token = tokens[4];
    } else if (count == 4U && equals(line_.data(), tokens[1], "import") &&
               equals(line_.data(), tokens[2], "chunk")) {
      command.kind = CommandKind::ProfileImportChunk;
      valid = copy_token(line_.data(), tokens[3], command.value);
    } else if (count == 5U && equals(line_.data(), tokens[1], "import") &&
               equals(line_.data(), tokens[2], "chunk")) {
      command.kind = CommandKind::ProfileImportChunk;
      valid = copy_token(line_.data(), tokens[4], command.value);
      numeric = true;
      numeric_token = tokens[3];
    } else if (count == 4U && equals(line_.data(), tokens[1], "import") &&
               equals(line_.data(), tokens[2], "finish")) {
      command.kind = CommandKind::ProfileImportFinish;
      valid = copy_token(line_.data(), tokens[3], command.target);
    } else if (count == 3U && equals(line_.data(), tokens[1], "rollback")) {
      command.kind = CommandKind::ProfileRollback;
      valid = copy_token(line_.data(), tokens[2], command.target);
    } else if (count == 4U && equals(line_.data(), tokens[1], "reset")) {
      command.kind = CommandKind::ProfileReset;
      valid = copy_token(line_.data(), tokens[2], command.target) &&
              equals(line_.data(), tokens[3], "CONFIRM");
    } else if (count == 3U && equals(line_.data(), tokens[1], "validate")) {
      command.kind = CommandKind::ProfileValidate;
      valid = copy_token(line_.data(), tokens[2], command.target);
    } else if (count == 3U && equals(line_.data(), tokens[1], "preview")) {
      command.kind = CommandKind::ProfilePreview;
      valid = copy_token(line_.data(), tokens[2], command.target);
    } else if (count == 3U && equals(line_.data(), tokens[1], "commit")) {
      command.kind = CommandKind::ProfileCommit;
      valid = copy_token(line_.data(), tokens[2], command.target);
    } else {
      valid = false;
    }
  } else {
    event.status = CommandStatus::UnknownCommand;
    reset();
    return true;
  }

  if (!valid) {
    event.status = CommandStatus::InvalidSyntax;
    reset();
    return true;
  }
  if (numeric) {
    if (!parse_integer(line_.data(), numeric_token, command.numeric_value)) {
      event.status = CommandStatus::InvalidNumeric;
      reset();
      return true;
    }
    command.has_numeric = true;
  }
  event.status = CommandStatus::Accepted;
  event.command = command;
  reset();
  return true;
}

bool MaintenanceResponse::append(std::string_view value) noexcept {
  if (value.size() > kMaxResponseBytes - size_) return false;
  for (std::size_t i = 0U; i < value.size(); ++i) bytes_[size_ + i] = value[i];
  size_ += value.size();
  bytes_[size_] = '\0';
  return true;
}

bool MaintenanceResponse::append_status(CommandStatus status) noexcept {
  return append(status == CommandStatus::Accepted ? "OK\n" : "ERR ") &&
         (status == CommandStatus::Accepted || append(status_name(status))) &&
         (status == CommandStatus::Accepted || append("\n"));
}

bool MaintenanceResponse::append_secret(std::string_view label,
                                        std::string_view) noexcept {
  return append(label) && append("=[REDACTED]\n");
}

bool MaintenanceResponse::append_presence(std::string_view label,
                                          bool present) noexcept {
  return append(label) && append(present ? "=present\n" : "=absent\n");
}

MaintenancePollStats MaintenanceService::poll(std::uint64_t now_us) noexcept {
  MaintenancePollStats stats{};
  sink_.poll(now_us);
  const bool connected = transport_.connected();
  if (connected_last_ && !connected) {
    parser_.reset();
    sink_.transport_lost();
  }
  connected_last_ = connected;
  while (transport_.available() && stats.bytes_read < kMaxBytesPerPoll) {
    const int value = transport_.read();
    if (value < 0) break;
    ++stats.bytes_read;
    ParseEvent event{};
    if (parser_.consume(static_cast<char>(value), event)) {
      dispatch(event, now_us, stats);
    }
  }
  sink_.poll(now_us);
  return stats;
}

void MaintenanceService::dispatch(ParseEvent& event,
                                  std::uint64_t now_us,
                                  MaintenancePollStats& stats) noexcept {
  MaintenanceResponse response{};
  if (event.status == CommandStatus::Accepted) {
    ++stats.commands_dispatched;
    ++commands_dispatched_;
    const CommandStatus result = sink_.execute(event.command, now_us, response);
    if (response.empty()) response.append_status(result);
  } else {
    ++stats.parser_errors;
    response.append_status(event.status);
  }
  event.command.clear_sensitive();
  if (!response.empty() && !transport_.write(response.view())) {
    stats.transport_write_failed = true;
  }
}

}  // namespace sigap::maintenance
