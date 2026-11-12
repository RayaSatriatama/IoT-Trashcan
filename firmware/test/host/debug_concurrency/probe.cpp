#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

#include "debug/service.h"

namespace {
class MutexLock final : public sigap::debug::IDebugLock {
 public:
  void lock() noexcept override { mutex_.lock(); }
  void unlock() noexcept override { mutex_.unlock(); }
 private:
  std::mutex mutex_;
};
sigap::acquisition::BootId make_boot() {
  sigap::acquisition::BootId boot{};
  boot.present = true;
  boot.bytes[15] = 1U;
  return boot;
}
}
int main() {
  using namespace sigap;
  MutexLock lock;
  debug::DebugService service(make_boot(), "HOST-MUTEX", debug::Level::Info, lock);
  std::atomic<bool> done{false}, valid{true}, observer_active{false};
  std::atomic<unsigned> produced{0U}, observed{0U};
  std::thread producer([&] {
    for (uint64_t n = 1U; n <= 6000U; ++n) {
      debug::EventInput input{};
      input.monotonic_us = n;
      input.module = debug::Module::Runtime;
      input.event_code = 1U;
      input.level = debug::Level::Error;
      if (!service.emit(input)) valid.store(false, std::memory_order_relaxed);
      else produced.fetch_add(1U, std::memory_order_relaxed);
      if (n == 1U) {
        while (!observer_active.load(std::memory_order_acquire)) std::this_thread::yield();
      }
    }
    done.store(true, std::memory_order_release);
  });
  std::thread observer([&] {
    observer_active.store(true, std::memory_order_release);
    uint64_t cursor_after = 0U, last_seen = 0U;
    do {
      const auto meta = service.meta(UINT64_MAX);
      if (meta.latest < meta.oldest && meta.latest != 0U) valid.store(false, std::memory_order_relaxed);
      debug::BatchSnapshot snapshot{};
      if (!service.capture({make_boot(), cursor_after}, 8U, UINT64_MAX, snapshot)) {
        valid.store(false, std::memory_order_relaxed);
        continue;
      }
      if (snapshot.count > 8U || (snapshot.meta.latest < snapshot.meta.oldest && snapshot.meta.latest != 0U))
        valid.store(false, std::memory_order_relaxed);
      for (size_t i = 0U; i < snapshot.count; ++i) {
        const auto& event = snapshot.events[i];
        if (event.sequence <= last_seen || event.sequence > snapshot.meta.latest ||
            event.monotonic_us > snapshot.meta.captured_monotonic_us)
          valid.store(false, std::memory_order_relaxed);
        last_seen = event.sequence;
        cursor_after = event.sequence;
        observed.fetch_add(1U, std::memory_order_relaxed);
      }
      const auto page = service.read({make_boot(), cursor_after});
      if (page.has_event && page.event.sequence <= cursor_after) valid.store(false, std::memory_order_relaxed);
      service.set_level((cursor_after & 1U) == 0U ? debug::Level::Debug : debug::Level::Info);
      service.note_usb_loss(1U);
      service.note_delivery_loss(1U);
    } while (!done.load(std::memory_order_acquire));
    for (;;) {
      debug::BatchSnapshot snapshot{};
      if (!service.capture({make_boot(), cursor_after}, 8U, UINT64_MAX, snapshot)) {
        valid.store(false, std::memory_order_relaxed);
        break;
      }
      if (snapshot.count == 0U) break;
      if (snapshot.gap && cursor_after + 1U < snapshot.meta.oldest) cursor_after = snapshot.meta.oldest - 1U;
      for (size_t i = 0U; i < snapshot.count; ++i) {
        if (snapshot.events[i].sequence <= last_seen) valid.store(false, std::memory_order_relaxed);
        last_seen = snapshot.events[i].sequence;
        cursor_after = snapshot.events[i].sequence;
        observed.fetch_add(1U, std::memory_order_relaxed);
      }
    }
  });
  producer.join();
  observer.join();
  const auto final = service.meta(UINT64_MAX);
  if (!valid.load(std::memory_order_relaxed) || produced.load() != 6000U ||
      final.latest != 6000U || final.overwritten != 6000U - 64U ||
      final.delivery_lost == 0U || final.usb_lost == 0U) return 1;
  std::cout << "mutex_probe PASS produced=" << produced.load() << " observed=" << observed.load()
            << " latest=" << final.latest << " overwritten=" << final.overwritten
            << " record_bytes=" << sizeof(debug::EventRecord)
            << " ring_bytes=" << sizeof(debug::EventRecord) * 64U
            << " service_bytes=" << sizeof(debug::DebugService)
            << " snapshot_bytes=" << sizeof(debug::BatchSnapshot) << '\n';
  return 0;
}
