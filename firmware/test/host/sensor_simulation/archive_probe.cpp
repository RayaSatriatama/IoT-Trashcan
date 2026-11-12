// Actual application + fake SDK/filesystem; loopback HTTP bridge is host-only.
#include "sigap_app.h"
#include "fixtures.h"
#include "network/https_transport.h"
#include <cstdio>
#include <iostream>
#include <string>

namespace sigap::board { void log_runtime_board_memory() noexcept {} }
namespace sigap {
struct DebugAppProbe {
  static network::IImmutableBatchSource& source(SigapApp& app) { return app.network_source_; }
  static void stop(SigapApp& app) { app.acquisition_.disable(); }
  static bool pending(SigapApp& app) { return app.batch_runtime_.pending(); }
  static bool drained(SigapApp& app) {
    return app.archive_store_.pending_bytes() == 0U &&
           app.acquisition_.handoff().size() == 0U &&
           app.processing_runtime_.handoff().size() == 0U;
  }
};
}
namespace {
using namespace sigap;
struct Clock final : network::IMonotonicClock {
  uint64_t now_us() const noexcept override { return test_platform::now_us; }
};
struct Jitter final : network::IRandomJitter {
  uint32_t sample(uint32_t) noexcept override { return 0U; }
};
struct Link final : network::IWifiLink {
  bool connected() const noexcept override { return true; }
  bool poll_connect() noexcept override { return true; }
};
struct Transport final : network::IHttpTransport {
  std::size_t delivered{};
  uint16_t sensor_mask{};
  network::TransportResult post_batch(std::string_view body, std::string_view token,
      uint16_t count, uint32_t, network::NetworkHttpResponse& response) noexcept override {
    if (token != "TEST-DEVICE-TOKEN" || count != 1U) return network::TransportResult::Failed;
    constexpr const char* sensors[] = {"hx711_organic", "hx711_inorganic", "hx711_b3",
      "ultrasonic_organic", "ultrasonic_inorganic", "ultrasonic_b3", "mq4", "mq135",
      "mq7", "mq8", "ds18b20_b3", "sht31_44", "sht31_45"};
    for (std::size_t index = 0U; index < std::size(sensors); ++index) {
      const std::string needle = std::string("\"sensor_id\":\"") + sensors[index] + "\"";
      if (body.find(needle) != std::string_view::npos)
        sensor_mask = static_cast<uint16_t>(sensor_mask | (1U << index));
    }
    std::cout << "BATCH " << body << std::endl;
    std::string ack;
    if (!std::getline(std::cin, ack)) return network::TransportResult::Failed;
    response.status_code = 200;
    response.ack_present = network::decode_archive_ack(ack, response.ack);
    // The actual decoder must reject trailing JSON, without changing durable state.
    telemetry_archive::Ack rejected;
    if (network::decode_archive_ack(ack + "{}", rejected)) return network::TransportResult::Failed;
    ++delivered;
    return network::TransportResult::Response;
  }
};
int fail(const char* reason) { std::fprintf(stderr, "FAIL %s\n", reason); return 1; }
}
int main() {
  seed_test_boot();
  sigap::SigapApp app;
  app.setup();
  if (!app.acquisition().enabled()) return fail("simulation application not acquiring");
  Clock clock;
  Jitter jitter;
  Link link;
  Transport transport;
  sigap::network::NetworkWorker worker({}, clock, jitter, link, transport,
                                       sigap::DebugAppProbe::source(app));
  if (!worker.begin("TEST-DEVICE-TOKEN")) return fail("network worker begin");
  constexpr uint16_t kAllSensors{(1U << 13U) - 1U};
  bool acquisition_stopped{false};
  uint64_t step{0U};
  for (; step < 30000U; ++step) {
    test_platform::now_us = 1000U + step * 1000U;
    app.loop();
    if (sigap::DebugAppProbe::pending(app) &&
        worker.poll() != sigap::network::NetworkOutcome::Delivered)
      return fail("actual ACK failed network context/journal/reclaim validation");
    if (!acquisition_stopped && transport.sensor_mask == kAllSensors) {
      sigap::DebugAppProbe::stop(app);
      acquisition_stopped = true;
    }
    if (acquisition_stopped && sigap::DebugAppProbe::drained(app) &&
        !sigap::DebugAppProbe::pending(app)) break;
  }
  if (!acquisition_stopped || !sigap::DebugAppProbe::drained(app) ||
      sigap::DebugAppProbe::pending(app))
    return fail("application archive/handoff not drained after actual ACKs");
  if (app.acquisition().handoff().dropped_records() != 0U) return fail("sample loss");
  std::cout << "DONE " << transport.delivered << std::endl;
  return transport.delivered > 13U ? 0 : fail("insufficient producer records");
}
