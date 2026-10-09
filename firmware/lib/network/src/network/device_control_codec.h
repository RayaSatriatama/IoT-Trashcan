#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "network/device_control.h"

namespace sigap::network {

inline constexpr std::size_t kMaxDeviceControlWireBytes{
    kMaxDeviceControlPayloadBytes + 512U};

struct DeviceControlWireBody final {
  std::array<char, kMaxDeviceControlWireBytes> bytes{};
  std::size_t size{0U};

  std::string_view view() const noexcept {
    return std::string_view(bytes.data(), size);
  }
};

// The backend supplies expires_in_ms from its own claimed lease.  The absolute
// expires_at is retained for audit but is not used as a device deadline unless
// a trusted wall-clock anchor exists.
bool decode_device_control_command(std::string_view wire, uint64_t now_us,
                                   DeviceControlCommand& output) noexcept;

// Result bytes are produced once from device-confirmed state and retained by
// DeviceControlPoller's journal until the backend acknowledges them.
bool encode_device_control_result(const DeviceControlResult& result,
                                  DeviceControlWireBody& output) noexcept;

}  // namespace sigap::network
