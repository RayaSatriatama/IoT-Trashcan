#include "network/device_control_codec.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace sigap::network {
namespace {

class JsonCursor final {
 public:
  explicit JsonCursor(std::string_view input) noexcept : input_(input) {}

  void skip_space() noexcept {
    while (position_ < input_.size() &&
           (input_[position_] == ' ' || input_[position_] == '\n' ||
            input_[position_] == '\r' || input_[position_] == '\t')) {
      ++position_;
    }
  }

  bool take(char expected) noexcept {
    skip_space();
    if (position_ >= input_.size() || input_[position_] != expected) return false;
    ++position_;
    return true;
  }

  bool string(char* output, std::size_t capacity, std::size_t& size,
              bool allow_empty = false) noexcept {
    skip_space();
    if (output == nullptr || capacity == 0U || position_ >= input_.size() ||
        input_[position_] != '\"') {
      return false;
    }
    ++position_;
    size = 0U;
    while (position_ < input_.size() && input_[position_] != '\"') {
      const auto byte = static_cast<unsigned char>(input_[position_]);
      if (byte < 0x20U || input_[position_] == '\\' ||
          size + 1U >= capacity) {
        return false;
      }
      output[size++] = input_[position_++];
    }
    if (position_ >= input_.size() || (!allow_empty && size == 0U)) return false;
    output[size] = '\0';
    ++position_;
    return true;
  }

  bool uint32(uint32_t& output) noexcept {
    skip_space();
    if (position_ >= input_.size() || input_[position_] < '0' ||
        input_[position_] > '9') {
      return false;
    }
    uint64_t value{0U};
    const std::size_t begin = position_;
    while (position_ < input_.size() && input_[position_] >= '0' &&
           input_[position_] <= '9') {
      value = value * 10U + static_cast<uint64_t>(input_[position_] - '0');
      if (value > std::numeric_limits<uint32_t>::max()) return false;
      ++position_;
    }
    if (position_ - begin > 1U && input_[begin] == '0') return false;
    output = static_cast<uint32_t>(value);
    return true;
  }

  bool raw_value(char* output, std::size_t capacity, std::size_t& size,
                 bool terminate = true) noexcept {
    std::size_t begin{0U};
    if (!value_bounds(begin, size)) return false;
    if (output == nullptr || size > capacity ||
        (terminate && size >= capacity)) {
      return false;
    }
    std::memcpy(output, input_.data() + begin, size);
    if (terminate) output[size] = '\0';
    return true;
  }

  bool value_bounds(std::size_t& begin, std::size_t& size) noexcept {
    skip_space();
    if (position_ >= input_.size()) {
      return false;
    }
    begin = position_;
    char first = input_[position_];
    if (first == '\"') {
      ++position_;
      bool escaped{false};
      while (position_ < input_.size()) {
        const char value = input_[position_++];
        if (escaped) {
          escaped = false;
          continue;
        }
        if (value == '\\') {
          escaped = true;
        } else if (value == '\"') {
          break;
        } else if (static_cast<unsigned char>(value) < 0x20U) {
          return false;
        }
      }
      if (position_ > input_.size() || input_[position_ - 1U] != '\"') return false;
    } else if (first == '{' || first == '[') {
      uint16_t depth{0U};
      bool quoted{false};
      bool escaped{false};
      while (position_ < input_.size()) {
        const char value = input_[position_++];
        if (quoted) {
          if (escaped) {
            escaped = false;
          } else if (value == '\\') {
            escaped = true;
          } else if (value == '\"') {
            quoted = false;
          } else if (static_cast<unsigned char>(value) < 0x20U) {
            return false;
          }
          continue;
        }
        if (value == '\"') {
          quoted = true;
        } else if (value == '{' || value == '[') {
          if (++depth > 24U) return false;
        } else if (value == '}' || value == ']') {
          if (depth == 0U || --depth == 0U) break;
        }
      }
      if (depth != 0U || quoted) return false;
    } else {
      while (position_ < input_.size()) {
        const char value = input_[position_];
        if (value == ',' || value == '}' || value == ']' || value == ' ' ||
            value == '\n' || value == '\r' || value == '\t') {
          break;
        }
        ++position_;
      }
      if (position_ == begin) return false;
      const auto scalar = input_.substr(begin, position_ - begin);
      if (scalar != "true" && scalar != "false" && scalar != "null") {
        for (const char value : scalar) {
          if (!((value >= '0' && value <= '9') || value == '-' || value == '+' ||
                value == '.' || value == 'e' || value == 'E')) {
            return false;
          }
        }
      }
    }
    size = position_ - begin;
    return size != 0U;
  }

  bool complete() noexcept {
    skip_space();
    return position_ == input_.size();
  }

 private:
  std::string_view input_{};
  std::size_t position_{0U};
};

template <std::size_t N>
bool assign_text(DeviceControlText<N>& output, const char* value,
                 std::size_t size, bool allow_empty = false) noexcept {
  if (allow_empty && size == 0U) {
    output = DeviceControlText<N>{};
    return true;
  }
  return output.assign(std::string_view(value, size));
}

bool append(DeviceControlWireBody& output, std::string_view value) noexcept {
  if (output.size + value.size() >= output.bytes.size()) return false;
  std::copy(value.begin(), value.end(), output.bytes.begin() + output.size);
  output.size += value.size();
  output.bytes[output.size] = '\0';
  return true;
}

bool append_plain_json_string(DeviceControlWireBody& output,
                              std::string_view value) noexcept {
  if (value.empty() || value.find_first_of("\\\"\r\n\t") != std::string_view::npos) {
    return false;
  }
  return append(output, "\"") && append(output, value) && append(output, "\"");
}

const char* result_status(DeviceControlStatus status) noexcept {
  switch (status) {
    case DeviceControlStatus::Accepted: return "ACCEPTED";
    case DeviceControlStatus::Capturing: return "CAPTURING";
    case DeviceControlStatus::Validated: return "VALIDATED";
    case DeviceControlStatus::PreviewReady: return "PREVIEW_READY";
    case DeviceControlStatus::Committed: return "COMMITTED";
    case DeviceControlStatus::Cancelled: return "CANCELLED";
    case DeviceControlStatus::Failed:
    default: return "FAILED";
  }
}

}  // namespace

bool decode_device_control_command(std::string_view wire, uint64_t now_us,
                                   DeviceControlCommand& output) noexcept {
  output = DeviceControlCommand{};
  if (wire.empty() || wire.size() > kMaxDeviceControlWireBytes) return false;
  JsonCursor cursor{wire};
  if (!cursor.take('{')) return false;
  uint16_t seen{0U};
  uint32_t expiry_ms{0U};
  char text[129]{};
  std::size_t text_size{0U};
  std::size_t payload_size{0U};
  for (;;) {
    cursor.skip_space();
    if (cursor.take('}')) break;
    char key[32]{};
    std::size_t key_size{0U};
    if (!cursor.string(key, sizeof(key), key_size) || !cursor.take(':')) return false;
    const std::string_view field(key, key_size);
    uint16_t bit{0U};
    if (field == "command_id") bit = 1U;
    else if (field == "device_id") bit = 2U;
    else if (field == "operator_id") bit = 4U;
    else if (field == "session_id") bit = 8U;
    else if (field == "sensor_id") bit = 16U;
    else if (field == "operation") bit = 32U;
    else if (field == "payload") bit = 64U;
    else if (field == "expires_in_ms") bit = 128U;
    else if (field == "attempt") bit = 256U;
    else if (field == "request_hash") bit = 512U;
    else if (field == "expires_at" || field == "status") bit = 1024U;
    else return false;
    if ((seen & bit) != 0U) return false;
    seen = static_cast<uint16_t>(seen | bit);
    if (bit == 64U) {
      if (!cursor.raw_value(reinterpret_cast<char*>(output.payload.data()),
                            output.payload.size(), payload_size, false)) {
        return false;
      }
    } else if (bit == 128U || bit == 256U) {
      uint32_t value{0U};
      if (!cursor.uint32(value)) return false;
      if (bit == 128U) expiry_ms = value;
      else output.attempt = value;
    } else {
      const bool optional = bit == 8U || bit == 512U || bit == 1024U;
      if (!cursor.string(text, sizeof(text), text_size, optional)) return false;
      bool assigned{false};
      switch (bit) {
        case 1U: assigned = assign_text(output.command_id, text, text_size); break;
        case 2U: assigned = assign_text(output.device_id, text, text_size); break;
        case 4U: assigned = assign_text(output.operator_id, text, text_size); break;
        case 8U: assigned = assign_text(output.session_id, text, text_size, true); break;
        case 16U: assigned = assign_text(output.sensor_id, text, text_size); break;
        case 32U: assigned = assign_text(output.operation, text, text_size); break;
        case 512U: assigned = assign_text(output.request_hash, text, text_size, true); break;
        case 1024U: assigned = true; break;
        default: break;
      }
      if (!assigned) return false;
    }
    cursor.skip_space();
    if (cursor.take('}')) break;
    if (!cursor.take(',')) return false;
  }
  constexpr uint16_t kRequiredFields{1023U};
  if (!cursor.complete() || (seen & kRequiredFields) != kRequiredFields ||
      !device_control_deadline_from_relative_ms(now_us, expiry_ms,
                                                output.expires_at_us) ||
      payload_size == 0U || payload_size > output.payload.size() ||
      output.request_hash.view().size() != 71U ||
      output.request_hash.view().substr(0U, 7U) != "sha256:") {
    output = DeviceControlCommand{};
    return false;
  }
  output.payload_size = payload_size;
  return true;
}

bool encode_device_control_result(const DeviceControlResult& result,
                                  DeviceControlWireBody& output) noexcept {
  output = DeviceControlWireBody{};
  if (result.command_id.size == 0U || result.session_id.size == 0U ||
      result.payload_size == 0U || result.payload_size > result.payload.size()) {
    return false;
  }
  JsonCursor payload{std::string_view(
      reinterpret_cast<const char*>(result.payload.data()), result.payload_size)};
  std::size_t ignored_start{0U};
  std::size_t checked_size{0U};
  if (!payload.value_bounds(ignored_start, checked_size) ||
      checked_size != result.payload_size || !payload.complete()) {
    return false;
  }
  if (!append(output, "{\"status\":") ||
      !append_plain_json_string(output, result_status(result.status)) ||
      !append(output, ",\"session_id\":") ||
      !append_plain_json_string(output, result.session_id.view()) ||
      !append(output, ",\"result\":") ||
      !append(output, std::string_view(
          reinterpret_cast<const char*>(result.payload.data()), checked_size))) {
    return false;
  }
  if (result.error_code.size != 0U &&
      (!append(output, ",\"error_code\":") ||
       !append_plain_json_string(output, result.error_code.view()))) {
    return false;
  }
  return append(output, "}");
}

}  // namespace sigap::network
