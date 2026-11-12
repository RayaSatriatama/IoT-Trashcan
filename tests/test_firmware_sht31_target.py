"""Portable actual-header SHT31 target and graph checks using host SDK fakes."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware"


class PhysicalSht31(unittest.TestCase):
    def test_actual_target_header_and_physical_graph(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as directory:
            probe = Path(directory)
            include = probe / "include"
            include.mkdir()
            (include / "Wire.h").write_text(r'''#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
struct TwoWire {
 bool initialized=true; int end_status=0; size_t request_size=6, cursor=0; uint8_t address=0;
 std::array<uint8_t,6> frame{}; unsigned begins=0, requests=0; uint8_t commands[2]{}; unsigned writes=0;
 bool begin(int,int){++begins;return initialized;} void setTimeOut(uint16_t){}
 void beginTransmission(uint8_t a){address=a;writes=0;}
 size_t write(uint8_t b){if(writes<2)commands[writes]=b;++writes;return 1;}
 uint8_t endTransmission(bool){return static_cast<uint8_t>(end_status);}
 size_t requestFrom(uint8_t a,size_t n,bool){++requests;cursor=0;return a==address&&n==6?request_size:0;}
 int available(){return cursor<frame.size()?1:0;} int read(){return frame[cursor++];}
};
inline TwoWire Wire;
''')
            (include / "esp_timer.h").write_text("#pragma once\n#include <cstdint>\ninline int64_t sht_now_us=0; inline int64_t esp_timer_get_time(){return sht_now_us;}\n")
            (probe / "probe.cpp").write_text(r'''#include <array>
#include <cassert>
#include <cstdio>
#include "acquisition/target_runtime.h"
using namespace sigap::acquisition;
void graph_handoff(bool nack) {
 Wire=TwoWire{}; Wire.end_status=nack?2:0;
 Wire.frame={0x66,0x66,0,0x80,0,0};
 Wire.frame[2]=sht31_crc8(Wire.frame.data(),2);
 Wire.frame[5]=sht31_crc8(Wire.frame.data()+3,2);
 sht_now_us=0; sensor_io::now_us=0;
 BootId boot{};boot.present=true;boot.bytes[0]=1;
 DeviceId device{};device.present=true;device.bytes[0]=1;
 ConfigRef config{};config.present=true;config.schema_version=1;config.config_version=1;config.hash[0]=1;
 SequenceAllocator sequence;
 RecordFactory factory(sequence,RecordContext{boot,device,config,nullptr});
 TargetProfileSet profiles{};
 Sht31Profile invalid_profile{};
 profiles.sht31[0]=&invalid_profile;
 SampleRing<TargetAcquisition::kHandoffCapacity> handoff;
 TargetAcquisition acquisition(factory,sigap::board::kN16R8Pins,profiles,handoff);
 sigap::config::SamplingConfig sampling{};
 sampling.hx711_period_ms=100; sampling.ultrasonic_period_ms=100;
 sampling.mq_period_ms=100; sampling.sht31_period_ms=20;
 assert(acquisition.begin(sampling,0)); assert(acquisition.registered_jobs()==4);
 std::array<bool,13> seen{}; unsigned sht_records=0;
 for(uint64_t now=0;now<=2500000;now+=1000) {
  sht_now_us=static_cast<int64_t>(now); sensor_io::now_us=now;
  acquisition.poll(now); SampleRecord sample{};
  while(handoff.try_pop(sample)==HandoffStatus::Dequeued) {
   const auto sensor=sample.header.sensor;
   assert(sensor==SensorId::Hx711Organic || sensor==SensorId::UltrasonicOrganic ||
          sensor==SensorId::Mq135 || sensor==SensorId::Sht31_44);
   seen[static_cast<unsigned>(sensor)]=true;
   if(sensor!=SensorId::Sht31_44) continue;
   ++sht_records;
   assert(!sample.converted.primary_available && !sample.converted.secondary_available);
   if(nack) {
    assert(sample.quality==SampleQuality::Timeout && sample.reason==SampleReason::Timeout);
    assert(!sample.raw.present && !sample.raw.sht31_ticks_available);
   } else {
    assert(now>=15000); // A pending first conversion must not fabricate a sample.
    assert(sample.quality==SampleQuality::Uncalibrated);
    assert(sample.reason==SampleReason::InvalidProfile);
    assert(sample.raw.present && sample.raw.sht31_ticks_available && sample.raw.crc_valid);
    assert(sample.raw.sht31_temperature_ticks==0x6666 && sample.raw.sht31_humidity_ticks==0x8000);
   }
  }
 }
 assert(sht_records>0 && !sensor_io::forbidden);
 for(auto sensor:{SensorId::Hx711Organic,SensorId::UltrasonicOrganic,SensorId::Mq135,SensorId::Sht31_44})
  assert(seen[static_cast<unsigned>(sensor)]);
 acquisition.disable();
}
int main(){
 TargetSht31Reader reader(8,9); assert(Wire.begins==1);
 Wire.frame={0x66,0x66,0,0x80,0,0}; Wire.frame[2]=sht31_crc8(Wire.frame.data(),2); Wire.frame[5]=sht31_crc8(Wire.frame.data()+3,2);
 auto p=reader.read(0x44); assert(p.pending&&!p.ok&&Wire.commands[0]==0x24&&Wire.commands[1]==0);
 sht_now_us=14999; p=reader.read(0x44); assert(p.pending&&!p.ok&&Wire.requests==0);
 sht_now_us=15000; auto good=reader.read(0x44); assert(good.ok&&good.crc_valid&&good.temperature_ticks==0x6666);
 Wire.frame[2]^=1; auto next=reader.read(0x44); assert(next.pending); sht_now_us+=15000; auto bad=reader.read(0x44); assert(bad.ok&&!bad.crc_valid&&!bad.temperature_crc_valid&&bad.humidity_crc_valid);
 Wire.end_status=2; assert(!reader.read(0x45).ok);
 Wire.end_status=0; assert(reader.read(0x45).pending); Wire.request_size=5; sht_now_us+=15000; assert(!reader.read(0x45).ok);
 assert(!reader.read(0x46).ok);
 Wire=TwoWire{}; Wire.initialized=false;
 TargetSht31Reader failed(8,9); auto unavailable=failed.read(0x44);
 assert(!unavailable.ok && !unavailable.pending);
 graph_handoff(false); graph_handoff(true);
 puts("PASS actual-header decoder pending CRC NACK short address graph factory handoff");
}
''')
            includes = [include, FW / "test/host/physical_reference/include",
                        FW / "test/host/sensor_simulation/include", FW / "include"]
            includes += sorted((FW / "lib").glob("*/src"))
            binary = probe / "probe"
            cmd = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DARDUINO",
                   "-DSIGAP_SENSOR_SIMULATION=0", "-DSIGAP_PHYSICAL_REFERENCE_ONLY=1"]
            cmd += ["-I" + str(p) for p in includes]
            cmd += [str(probe / "probe.cpp"), "-o", str(binary)]
            subprocess.run(cmd, cwd=ROOT, check=True, timeout=60)
            result = subprocess.run([str(binary)], cwd=ROOT, check=True, capture_output=True,
                                    text=True, timeout=15)
            self.assertIn("PASS actual-header decoder", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
