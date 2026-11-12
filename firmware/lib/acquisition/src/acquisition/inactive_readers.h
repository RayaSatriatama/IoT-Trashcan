#pragma once
#include "acquisition/sensors/hx711.h"
#include "acquisition/sensors/ultrasonic.h"
#include "acquisition/sensors/mq_adc.h"
#include "acquisition/sensors/ds18b20.h"
namespace sigap::acquisition {
// Inactive channels are never scheduled and never touch GPIO. No synthetic samples.
class InactiveHx711Reader final : public IHx711Reader {
 public:
  InactiveHx711Reader(uint8_t,uint8_t) noexcept {}
  bool ready() noexcept override { return false; }
  Hx711ReadResult read() noexcept override { return {}; }
};
class InactiveUltrasonicReader final : public IUltrasonicReader {
 public:
  InactiveUltrasonicReader(uint8_t,uint8_t) noexcept {}
  bool trigger(uint64_t) noexcept override { return false; }
  UltrasonicEchoResult poll(uint64_t) noexcept override { return {}; }
  void cancel() noexcept override {}
};
class InactiveMqAdcReader final : public IMqAdcReader {
 public:
  explicit InactiveMqAdcReader(uint8_t) noexcept {}
  MqAdcReadResult read() noexcept override { return {}; }
};
class InactiveDs18b20Reader final : public IDs18b20Reader {
 public:
  explicit InactiveDs18b20Reader(uint8_t,const Ds18b20Profile* = nullptr) noexcept {}
  void set_profile(const Ds18b20Profile*) noexcept {}
  bool begin() noexcept override { return false; }
  bool apply_profile(const Ds18b20Profile*) noexcept override { return false; }
  bool request_conversion() noexcept override { return false; }
  Ds18b20ReadResult read_conversion() noexcept override { return {}; }
};
class InactiveHeaterRail final : public IMq7HeaterRail {
 public:
  explicit InactiveHeaterRail(uint8_t) noexcept {}
  bool select_high() noexcept override { return false; }
  bool select_low() noexcept override { return false; }
};
} // namespace sigap::acquisition
