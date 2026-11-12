#pragma once
#include <cstddef>
#include <string_view>
#include "debug/types.h"
namespace sigap::debug {
// Encoders write exactly len bytes; no trailing NUL is included or required.
// On failure len is zero. Capacity equal to the payload size is sufficient.
bool encode_meta(const Meta&, char*, size_t, size_t&) noexcept;
bool encode_page(const Page&, char*, size_t, size_t&) noexcept;
bool encode_event_json(const EventRecord&, char*, size_t, size_t&) noexcept;
bool encode_upload(std::string_view device_id, const BatchSnapshot&, char*, size_t, size_t&) noexcept;
bool decode_upload_ack(std::string_view, UploadAck&) noexcept;
// Caller freezes/validates the request snapshot and hashes its exact transmitted bytes.
// The digest is caller-computed; this helper only checks ACK context against both.
bool matches_upload_ack(const UploadAck&, std::string_view expected_device_id,
                        const BatchSnapshot& sent,
                        const std::array<uint8_t,32>& expected_digest) noexcept;
}
