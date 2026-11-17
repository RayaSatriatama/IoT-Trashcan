#pragma once
#include <cstdint>
#ifdef ARDUINO
#include <Arduino.h>
#include <esp_timer.h>
#include <cstdio>
#endif
namespace sigap::acquisition {
#ifdef ARDUINO
enum class UsbPhase : uint8_t { Init, Wifi, Ble, Maintenance, Filesystem, Profiles, Acquisition, Emitter, Drain, StorageSlice, StorageBatch, Migration, ArchiveHttp, Calibration, ControlHttp, DebugWorker, UsbSink, Idle, Count };
struct UsbPhaseState {
 uint32_t max_us[static_cast<uint8_t>(UsbPhase::Count)]{};
 uint64_t previous_us{0}, last_loop_entry_us{0}, max_loop_gap_us{0}; uint32_t loops{0}; uint8_t phase{255}; bool clock_valid{false}, loop_entry_valid{false};
};
inline UsbPhaseState& usb_phase_state() noexcept { static UsbPhaseState s{}; return s; }
inline uint64_t usb_phase_now() noexcept { const int64_t t=esp_timer_get_time(); return t<0?0U:static_cast<uint64_t>(t); }
inline void usb_phase_mark(UsbPhase next) noexcept {
 auto& s=usb_phase_state(); uint64_t now=usb_phase_now();
 if(s.clock_valid && now<s.previous_us) { s.previous_us=now; s.phase=static_cast<uint8_t>(next); return; }
 if(s.clock_valid && s.phase<static_cast<uint8_t>(UsbPhase::Count)) { uint64_t d=now-s.previous_us; uint32_t v=d>UINT32_MAX?UINT32_MAX:static_cast<uint32_t>(d); auto& m=s.max_us[s.phase]; if(v>m)m=v; }
 s.previous_us=now; s.phase=static_cast<uint8_t>(next); s.clock_valid=true;
}
inline void usb_phase_begin_loop() noexcept {
 auto& s=usb_phase_state(); uint64_t now=usb_phase_now();
 if(s.loop_entry_valid && now>=s.last_loop_entry_us) { uint64_t d=now-s.last_loop_entry_us; if(d>s.max_loop_gap_us)s.max_loop_gap_us=d; }
 s.last_loop_entry_us=now; s.loop_entry_valid=true; ++s.loops; s.previous_us=now; s.phase=static_cast<uint8_t>(UsbPhase::Init); s.clock_valid=true;
}
inline void emit_usb_sensor_debug(bool initialized, uint32_t calls, uint32_t completed, uint8_t end_code, uint8_t bytes, bool tc, bool hc, uint32_t trig, uint32_t polls, uint32_t high, uint32_t ready, uint32_t width, uint64_t gap) noexcept {
 static uint8_t frame=0; char line[240]; int n=0; auto& s=usb_phase_state();
 if(frame==0) n=snprintf(line,sizeof(line),"SIGAP_SENS v=4 init=%u sht_call=%lu sht_ok=%lu end=%u bytes=%u tc=%u hc=%u us_trig=%lu us_poll=%lu us_high=%lu us_ready=%lu width=%lu gap=%llu\n",initialized,(unsigned long)calls,(unsigned long)completed,end_code,bytes,tc,hc,(unsigned long)trig,(unsigned long)polls,(unsigned long)high,(unsigned long)ready,(unsigned long)width,(unsigned long long)gap);
 else if(frame==1) n=snprintf(line,sizeof(line),"SIGAP_PHASE v=5 g=1 lp=%lu lg=%llu p0=%lu p1=%lu p2=%lu p3=%lu p4=%lu p5=%lu\n",(unsigned long)s.loops,(unsigned long long)s.max_loop_gap_us,(unsigned long)s.max_us[0],(unsigned long)s.max_us[1],(unsigned long)s.max_us[2],(unsigned long)s.max_us[3],(unsigned long)s.max_us[4],(unsigned long)s.max_us[5]);
 else n=snprintf(line,sizeof(line),"SIGAP_PHASE v=5 g=2 lp=%lu lg=%llu p6=%lu p7=%lu p8=%lu p9=%lu p10=%lu p11=%lu p12=%lu p13=%lu p14=%lu p15=%lu p16=%lu p17=%lu\n",(unsigned long)s.loops,(unsigned long long)s.max_loop_gap_us,(unsigned long)s.max_us[6],(unsigned long)s.max_us[7],(unsigned long)s.max_us[8],(unsigned long)s.max_us[9],(unsigned long)s.max_us[10],(unsigned long)s.max_us[11],(unsigned long)s.max_us[12],(unsigned long)s.max_us[13],(unsigned long)s.max_us[14],(unsigned long)s.max_us[15],(unsigned long)s.max_us[16],(unsigned long)s.max_us[17]);
 if(n>0 && n<(int)sizeof(line) && Serial.availableForWrite()>=n) Serial.write((const uint8_t*)line,n);
 frame=(frame+1U)%3U;
}
#endif
}  // namespace sigap::acquisition
