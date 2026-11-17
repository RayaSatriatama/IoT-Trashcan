#include <cstdio>
#include "acquisition/sensors/hx711.h"
using namespace sigap::acquisition;
struct Reader final : IHx711Reader {
  int64_t counts{-25};
  bool ready() noexcept override { return true; }
  Hx711ReadResult read() noexcept override {
    return {true, false, counts, 1, 1, SampleReason::DataReady};
  }
};
int main() {
  SequenceAllocator sequence;
  RecordContext context;
  RecordFactory factory(sequence, context);
  Reader reader;
  Hx711Profile profile;
  profile.manifest.validity = ProfileValidity::Valid;
  profile.manifest.target_id = static_cast<uint32_t>(ProfileTarget::Hx711Organic);
  profile.counts_per_kg = 1000.0F;
  Hx711Acquirer sensor(SensorId::Hx711Organic, StreamId::Organic, reader, factory, &profile);
  SampleRecord sample;
  sensor.poll(1, sample);
  if (!sample.converted.primary_available || sample.converted.primary_value != 0.0F ||
      sample.raw.signed_counts != -25) {
    std::printf("FAIL weight floor: raw=%lld kg=%.6f\n", static_cast<long long>(sample.raw.signed_counts), sample.converted.primary_value);
    return 1;
  }
  reader.counts = 12000;
  sensor.poll(2, sample);
  if (sample.converted.primary_value != 12.0F) return 2;
  Hx711Acquirer missing(SensorId::Hx711Organic, StreamId::Organic, reader, factory, nullptr);
  missing.poll(3, sample);
  if (sample.converted.primary_available || sample.quality != SampleQuality::Uncalibrated) return 3;
  std::puts("PASS 3 weight scenarios: floor-zero, positive preserved, unavailable stays unavailable");
}
