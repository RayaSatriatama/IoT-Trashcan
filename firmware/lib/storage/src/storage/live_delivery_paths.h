#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "acquisition/acquisition_types.h"

namespace sigap::storage::live_delivery {
#if defined(SIGAP_FRESH_WEB_PRIORITY14) && SIGAP_FRESH_WEB_PRIORITY14
inline constexpr bool kPriorityFresh{true};
inline constexpr const char* kArchive = "/sigap/web-sync-v3.bin";
inline constexpr const char* kCheckpoint = "/sigap/web-sync-v3.ckp";
inline constexpr const char* kGaps = "/sigap/web-sync-v3.gaps";
inline constexpr const char* kBatches = "/sigap/web-sync-v3.batches";
inline constexpr const char* kBatchCheckpoint = "/sigap/web-sync-v3.batches.ckp";
inline constexpr const char* kAcks = "/sigap/web-sync-v3.acks";
inline constexpr const char* kAckCheckpoint = "/sigap/web-sync-v3.acks.ckp";
#else
inline constexpr bool kPriorityFresh{false};
inline constexpr const char* kArchive = "/sigap/live-v3.bin";
inline constexpr const char* kCheckpoint = "/sigap/live-v3.ckp";
inline constexpr const char* kGaps = "/sigap/live-v3.gaps";
inline constexpr const char* kBatches = "/sigap/live-v3.batches";
inline constexpr const char* kBatchCheckpoint = "/sigap/live-v3.batches.ckp";
inline constexpr const char* kAcks = "/sigap/live-v3.acks";
inline constexpr const char* kAckCheckpoint = "/sigap/live-v3.acks.ckp";
#endif

inline const char* related_suffix(const char* path, const char* suffix) noexcept {
  constexpr char prefix[] = "/sigap/";
  constexpr std::size_t prefix_length = sizeof(prefix) - 1U;
  constexpr std::size_t token_length = 22U;
  if (suffix == nullptr || std::strcmp(suffix, ".migrate.bak") != 0 ||
      !kPriorityFresh || path == nullptr || std::strncmp(path, prefix, prefix_length) != 0 ||
      std::strlen(path) != prefix_length + token_length + 2U ||
      path[prefix_length + token_length] != '.' ||
      path[prefix_length + token_length + 1U] != 's') return suffix;
  constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  for (std::size_t i = 0U; i < token_length; ++i) {
    if (std::strchr(alphabet, path[prefix_length + i]) == nullptr) return suffix;
  }
  const char final = path[prefix_length + token_length - 1U];
  if (final != 'A' && final != 'Q' && final != 'g' && final != 'w') return suffix;
  return ".mb";
}

struct BootPaths final {
  static constexpr std::size_t kPathSize{64U};
  std::array<char, kPathSize> archive{};
  std::array<char, kPathSize> checkpoint{};
  std::array<char, kPathSize> gaps{};
  std::array<char, kPathSize> batches{};
  std::array<char, kPathSize> batch_checkpoint{};
  std::array<char, kPathSize> acks{};
  std::array<char, kPathSize> ack_checkpoint{};
  bool valid{false};

  explicit BootPaths(const acquisition::BootId& boot) noexcept {
    if (!kPriorityFresh) {
      valid = copy(archive, kArchive) && copy(checkpoint, kCheckpoint) &&
          copy(gaps, kGaps) && copy(batches, kBatches) &&
          copy(batch_checkpoint, kBatchCheckpoint) && copy(acks, kAcks) &&
          copy(ack_checkpoint, kAckCheckpoint);
      return;
    }
    if (!boot.present) return;
    bool nonzero{false};
    for (uint8_t byte : boot.bytes) nonzero = nonzero || byte != 0U;
    if (!nonzero) return;
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    char token[23]{};
    uint32_t accumulator{0U};
    unsigned bits{0U};
    size_t output{0U};
    for (uint8_t byte : boot.bytes) {
      accumulator = (accumulator << 8U) | byte;
      bits += 8U;
      while (bits >= 6U) {
        bits -= 6U;
        token[output++] = alphabet[(accumulator >> bits) & 0x3fU];
      }
    }
    if (bits != 0U) token[output++] = alphabet[(accumulator << (6U - bits)) & 0x3fU];
    if (output != 22U) return;
    token[output] = '\0';
    char stem[32]{};
    const int stem_length = std::snprintf(stem, sizeof(stem), "/sigap/%s", token);
    if (stem_length <= 0 || static_cast<size_t>(stem_length) >= sizeof(stem)) return;
    valid = make(archive, stem, ".s") && make(checkpoint, stem, ".c") &&
        make(gaps, stem, ".g") && make(batches, stem, ".b") &&
        make(batch_checkpoint, stem, ".d") && make(acks, stem, ".a") &&
        make(ack_checkpoint, stem, ".k");
  }

 private:
  static bool copy(std::array<char, kPathSize>& out, const char* value) noexcept {
    const int n = std::snprintf(out.data(), out.size(), "%s", value);
    return n > 0 && static_cast<std::size_t>(n) < out.size();
  }
  static bool make(std::array<char, kPathSize>& out, const char* stem,
                   const char* suffix) noexcept {
    const int n = std::snprintf(out.data(), out.size(), "%s%s", stem, suffix);
    return n > 0 && static_cast<std::size_t>(n) < out.size();
  }
};
}  // namespace sigap::storage::live_delivery
