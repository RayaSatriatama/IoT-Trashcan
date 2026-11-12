#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "acquisition/acquisition_types.h"
namespace sigap::debug {
enum class Level : uint8_t { Error=0, Warn=1, Info=2, Debug=3 };
enum class Module : uint8_t { Runtime=1, Ble=2, Wifi=3, Storage=4, Acquisition=5, Calibration=6, Https=7 };
struct EventRecord { uint64_t sequence{}, monotonic_us{}, related_sequence{}; uint32_t request_id{}, duration_us{}; int32_t arg0{}, arg1{}; uint16_t event_code{}; Level level{Level::Info}; Module module{Module::Runtime}; uint8_t presence{}; };
struct EventInput { uint64_t monotonic_us{}, related_sequence{}; uint32_t request_id{}, duration_us{}; int32_t arg0{}, arg1{}; uint16_t event_code{}; Level level{Level::Info}; Module module{Module::Runtime}; uint8_t presence{}; };
inline constexpr uint8_t kHasRequestId=1U, kHasRelatedSequence=2U;
struct Cursor { acquisition::BootId boot{}; uint64_t after{}; };
struct Meta { acquisition::BootId boot{}; std::array<char,97> build_id{}; Level level{Level::Info}; uint64_t oldest{},latest{},overwritten{},delivery_lost{},usb_lost{},captured_monotonic_us{}; bool sequence_exhausted{}; };
struct Page { acquisition::BootId boot{}; Level level{Level::Info}; uint64_t oldest{},next_after{},overwritten{}; bool gap{},boot_changed{},has_event{}; EventRecord event{}; };
struct BatchSnapshot { Meta meta{}; bool gap{},boot_changed{}; std::array<EventRecord,8> events{}; size_t count{}; };
class IEventSink { public: virtual ~IEventSink()=default; virtual bool emit(const EventInput&) noexcept=0; };
struct UploadAck { uint8_t schema_version{}; std::array<char,8> status{}; std::array<char,65> device_id{}; uint8_t device_id_size{}; acquisition::BootId boot{}; uint64_t first_sequence{},last_sequence{}; uint8_t stored_count{},deduplicated_count{}; std::array<uint8_t,32> request_digest{}; };
static_assert(sizeof(EventRecord)<=64U,"debug event record exceeds RAM contract");
}
