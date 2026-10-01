#pragma once

#include <cstddef>
#include <cstdint>

namespace sigap::build {

inline constexpr char kBoardProfile[] = "n16r8";
inline constexpr uint32_t kConfigSchemaVersion{1};
inline constexpr uint32_t kTelemetrySchemaVersion{3};
inline constexpr uint16_t kMaxRecordsPerBatch{128};
inline constexpr std::size_t kMaxSerializedRecordBytes{16U * 1024U};
inline constexpr std::size_t kMaxBatchBodyBytes{32U * 1024U};

inline constexpr uint32_t kNvsOffset{0x9000};
inline constexpr uint32_t kNvsSize{0x5000};
inline constexpr uint32_t kPhyInitOffset{0xe000};
inline constexpr uint32_t kPhyInitSize{0x1000};
inline constexpr uint32_t kFactoryOffset{0x10000};
inline constexpr uint32_t kFactorySize{0x400000};
inline constexpr uint32_t kLittlefsOffset{0x410000};
inline constexpr uint32_t kLittlefsPartitionBytes{0x800000};
inline constexpr uint64_t kLittlefsReserveBytes{2ULL * 1024ULL * 1024ULL};
// Conservative pre-mount value supplied by the composition root. Runtime
// storage measurement belongs to the future filesystem/storage layer.
inline constexpr uint64_t kPreMountLittlefsValidationBytes{
    kLittlefsPartitionBytes};

constexpr bool partitions_fit_16mb() {
  return kNvsOffset + kNvsSize <= kPhyInitOffset &&
         kPhyInitOffset + kPhyInitSize <= kFactoryOffset &&
         kFactoryOffset + kFactorySize <= kLittlefsOffset &&
         kLittlefsOffset + kLittlefsPartitionBytes <= 16U * 1024U * 1024U;
}

static_assert(partitions_fit_16mb(), "SIGAP partitions must fit 16 MiB flash");

}  // namespace sigap::build
