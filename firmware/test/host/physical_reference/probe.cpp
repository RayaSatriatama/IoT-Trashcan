#include <array>
#include <cstdio>
#include <cstring>
#include "acquisition/target_runtime.h"
using namespace sigap::acquisition;
int main() {
  BootId boot{};boot.present=true;boot.bytes[0]=1;
  DeviceId device{};device.present=true;device.bytes[0]=1;
  ConfigRef config{};config.present=true;config.schema_version=1;config.config_version=1;config.hash[0]=1;
  SequenceAllocator sequence;RecordFactory factory(sequence,RecordContext{boot,device,config,nullptr});
  TargetProfileSet profiles{};SampleRing<TargetAcquisition::kHandoffCapacity> handoff;
  TargetAcquisition acquisition(factory,sigap::board::kN16R8Pins,profiles,handoff);
  if(sensor_io::forbidden){std::puts("FAIL construction touched non-reference GPIO/heater");return 1;}
  if(!acquisition.begin(sigap::config::SamplingConfig{},0)){std::puts("FAIL begin");return 2;}
  if(acquisition.registered_jobs()!=4){std::puts("FAIL expected exactly four acquisition jobs and no heater job");return 3;}
  std::array<bool,13> seen{};
  for(uint64_t now=0;now<=2500000;now+=1000){
    sensor_io::now_us=now;acquisition.poll(now);SampleRecord sample;
    while(handoff.try_pop(sample)==HandoffStatus::Dequeued){
      auto sensor=sample.header.sensor;
      if(sensor!=SensorId::Hx711Organic&&sensor!=SensorId::UltrasonicOrganic&&sensor!=SensorId::Mq135&&sensor!=SensorId::Sht31_44){std::puts("FAIL emitted excluded sensor");return 4;}
      seen[static_cast<unsigned>(sensor)]=true;
      if(sensor==SensorId::Mq135&&sample.raw.present&&sample.raw.adc_counts!=731){std::puts("FAIL not physical ADC fixture value");return 5;}
    }
  }
  if(!acquisition.refresh_profiles(2500001)){std::puts("FAIL refresh");return 6;}
  if(sensor_io::forbidden){std::puts("FAIL polling/profile refresh touched non-reference GPIO/heater");return 7;}
  for(auto sensor:{SensorId::Hx711Organic,SensorId::UltrasonicOrganic,SensorId::Mq135,SensorId::Sht31_44})if(!seen[static_cast<unsigned>(sensor)]){std::puts("FAIL reference sensor produced no quality record");return 8;}
  if(std::strcmp(sigap::build::kFirmwareBuildId,"sigap-task6-physical4-v1")!=0){std::puts("FAIL physical reference build ID");return 9;}
  acquisition.disable();
  std::printf("PASS physical reference channels=4 jobs=4 heaterIO=0 excludedGPIO=0 observedIO=%u\n",sensor_io::calls);
}
