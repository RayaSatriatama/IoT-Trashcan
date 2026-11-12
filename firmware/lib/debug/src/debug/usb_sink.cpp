#include "debug/usb_sink.h"
#include "debug/codec.h"
#include <algorithm>
#include <cstring>
namespace sigap::debug {
UsbDebugSink::UsbDebugSink(DebugService& service, IUsbDebugWriter& writer) noexcept
    : service_(service), writer_(writer) {}
void UsbDebugSink::poll(uint64_t now_us, size_t byte_budget) noexcept {
  if (byte_budget == 0U) return;
  if (!cursor_initialized_) {
    const Meta current = service_.meta(now_us);
    cursor_.boot = current.boot;
    cursor_.after = 0U;
    cursor_initialized_ = current.boot.present;
  }
  if (!cursor_initialized_) return;
  if (pending_size_ == 0U) {
    const Page page = service_.read(cursor_);
    if (page.boot_changed) {
      cursor_.boot = page.boot;
      cursor_.after = 0U;
      pending_event_ = false;
      return;
    }
    if (page.gap) {
      const uint64_t lost = page.oldest > cursor_.after ? page.oldest - cursor_.after - 1U : 0U;
      service_.note_usb_loss(lost);
      cursor_.after = page.oldest == 0U ? cursor_.after : page.oldest - 1U;
      return;
    }
    if (!page.has_event) return;
    char encoded[384U]{};
    size_t encoded_size = 0U;
    if (!encode_page(page, encoded, sizeof(encoded), encoded_size) ||
        encoded_size + 13U > pending_.size()) {
      service_.note_usb_loss(1U);
      cursor_.after = page.next_after;
      return;
    }
    constexpr char prefix[] = "SIGAP_DEBUG ";
    std::memcpy(pending_.data(), prefix, sizeof(prefix) - 1U);
    std::memcpy(pending_.data() + sizeof(prefix) - 1U, encoded, encoded_size);
    pending_size_ = sizeof(prefix) - 1U + encoded_size;
    pending_[pending_size_++] = '\n';
    pending_offset_ = 0U;
    pending_sequence_ = page.next_after;
    pending_event_ = true;
  }
  const size_t writable = writer_.writable();
  const size_t remaining = pending_size_ - pending_offset_;
  const size_t count = std::min(remaining, std::min(writable, byte_budget));
  if (count == 0U) return;
  const size_t written = std::min(count, writer_.write(pending_.data() + pending_offset_, count));
  pending_offset_ += written;
  if (pending_offset_ == pending_size_) {
    if (pending_event_) cursor_.after = pending_sequence_;
    pending_size_ = pending_offset_ = 0U;
    pending_event_ = false;
  }
}
}
