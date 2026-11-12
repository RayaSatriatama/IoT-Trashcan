#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include "NimBLEDevice.h"
#include "esp_timer.h"
#include "debug/service.h"
#include "provisioning/ble_provisioning.h"

using namespace sigap::provisioning;
bool host_fault=false;
extern "C" bool sigap_ble_diag_failed() noexcept { return host_fault; }
extern "C" bool sigap_ble_wifi_erase_attempted() noexcept { return host_fault; }
namespace {
struct Clock final : BleProvisioningClock {
  uint64_t now_us() const noexcept override { return static_cast<uint64_t>(host_now_us); }
};
struct Wifi final : IBleProvisioningBackend {
  int scans{},connects{},cancels{},lists{};
  std::string saved_ssid,saved_password;
  void (*connect_hook)(){};
  WifiState wifi_state() const noexcept override { return WifiState::Connected; }
  std::string_view current_ssid() const noexcept override { return "TEST"; }
  WifiScanStatus request_scan() noexcept override { assert(host_critical_depth==0); ++scans; return WifiScanStatus::Pending; }
  WifiScanStatus poll_scan(WifiNetwork*,size_t,size_t& count) noexcept override { count=0;return WifiScanStatus::Ready; }
  WifiScanStatus list_known_wifi(WifiNetwork*,size_t,size_t& count) noexcept override { assert(host_critical_depth==0);++lists;count=0;return WifiScanStatus::Ready; }
  bool request_connect(std::string_view ssid,std::string_view password) noexcept override {
    assert(host_critical_depth==0);++connects;saved_ssid=ssid;saved_password=password;
    if(connect_hook){const auto hook=connect_hook;connect_hook=nullptr;hook();}
    return true;
  }
  bool retry_wifi() noexcept override { assert(host_critical_depth==0);return true; }
  void cancel() noexcept override { assert(host_critical_depth==0);++cancels; }
};
struct Calibration final : IBleCalibrationBackend {
  int calls{},disconnects{};
  bool execute(std::string_view,char* output,size_t capacity,size_t& size) noexcept override {
    assert(host_critical_depth==0);++calls;constexpr std::string_view reply="OK CAL";
    assert(capacity>=reply.size());std::memcpy(output,reply.data(),reply.size());size=reply.size();return true;
  }
  void disconnect() noexcept override { assert(host_critical_depth==0);++disconnects; }
};
struct Fixture {
  Wifi wifi;Clock clock;Calibration calibration;
  ProvisioningPolicy policy;
  BleProvisioningSession session;
  ArduinoBleProvisioningService service;
  Fixture() : policy(make_policy()),session(wifi,clock,policy) {
    session.set_calibration_backend(&calibration);assert(service.begin(session,"SIGAP-TEST"));
  }
  static ProvisioningPolicy make_policy() { ProvisioningPolicy p;p.session_timeout_ms=10;return p; }
  NimBLECharacteristic& command() { return *host_server.service.characteristics[0]; }
  NimBLECharacteristic& response() { return *host_server.service.characteristics[1]; }
  void connect(uint16_t handle=7) { host_server.connect(handle);service.poll();assert(session.connected()); }
  void write(std::string_view value,uint16_t handle=7) { command().write(value,handle); }
};
void deferred() {
  Fixture f;host_server.connect(7);assert(!f.session.connected());f.write("SCAN_WIFI");assert(f.wifi.scans==0);
  f.service.poll();assert(f.session.connected());assert(f.wifi.scans==1);assert(f.calibration.calls==0);
  host_server.peer_disconnect(7);assert(f.session.connected());f.service.poll();assert(!f.session.connected());assert(f.wifi.cancels==1);
}
void bounded() {
  Fixture f;f.connect();for(int i=0;i<9;++i)f.write("CAL STATUS");assert(f.calibration.calls==0);
  f.service.poll();assert(f.calibration.calls==4);f.service.poll();assert(f.calibration.calls==8);f.service.poll();assert(f.calibration.calls==9);
  for(const auto& n:f.response().notifications){assert(n.bytes.size()<=20);assert(n.handle==7);}
}
void stale() {
  Fixture f;f.connect();f.write("CAL COMMIT TEST");host_server.peer_disconnect(7);host_server.connect(8);
  f.write("CAL COMMIT STALE",7);f.write("PING",8);assert(f.calibration.calls==0);f.service.poll();assert(f.calibration.calls==0);assert(f.session.connected());
  host_server.peer_disconnect(7);f.service.poll();assert(f.session.connected());
  f.write("CAL STATUS",8);f.service.poll();assert(f.calibration.calls==1);
  for(const auto& n:f.response().notifications)assert(n.handle==8);
}
void reused_handle() {
  Fixture f;f.connect();f.write("CAL COMMIT OLD");
  // The host model emits this genuine disconnect before reconnect.
  host_server.peer_disconnect(7);host_server.connect(7);
  f.write("PING");f.service.poll();assert(f.calibration.calls==0);assert(f.session.connected());
  // A disconnect callback for the live generation still closes it exactly once.
  host_server.peer_disconnect(7);f.service.poll();assert(!f.session.connected());
  f.service.poll();assert(!f.session.connected());
}
void delayed_disconnect() {
  Fixture f;f.connect(7);host_server.peer_disconnect(7);host_server.connect(7);f.service.poll();
  const int disconnects_before=f.calibration.disconnects;
  f.write("CAL STATUS",7);host_server.deliver_old_disconnect(7);f.service.poll();
  assert(f.session.connected());assert(f.calibration.calls==1);
  assert(f.calibration.disconnects==disconnects_before);assert(host_server.disconnects.empty());
}
void disconnect_during_query_hook(uint16_t handle, uint16_t mtu) {
  assert(handle==7);assert(mtu==23U);host_server.peer_disconnect(handle);
}
void disconnect_during_query() {
  Fixture f;f.connect(7);const std::string command="CONNECT "+std::string(32,'S')+" "+std::string(64,'P');f.write(command,7);host_mtu_hook=disconnect_during_query_hook;
  f.service.poll();assert(host_server.live_peers.empty());assert(f.wifi.connects==0);
  f.service.poll();assert(f.wifi.connects==0);assert(!f.session.connected());
  assert(f.calibration.disconnects==1);assert(f.wifi.cancels==1);
}
void enqueue_connects_from_backend() {host_server.peer_disconnect(7);}
void disconnect_between_writes() {
  Fixture f;f.connect(7);const std::string command="CONNECT "+std::string(32,'S')+" "+std::string(64,'P');f.write(command,7);f.write(command,7);f.wifi.connect_hook=enqueue_connects_from_backend;
  f.service.poll();assert(f.wifi.connects==1);assert(f.wifi.saved_ssid==std::string(32,'S'));assert(f.wifi.saved_password==std::string(64,'P'));
  assert(f.session.connected());f.service.poll();assert(f.wifi.connects==1);f.service.poll();assert(f.wifi.connects==1);
}
void stale_hint_between_writes() {
  Fixture f;f.connect(7);const std::string command="CONNECT "+std::string(32,'S')+" "+std::string(64,'P');f.write(command,7);f.write(command,7);f.wifi.connect_hook=[]{host_server.deliver_old_disconnect(7);};
  f.service.poll();assert(f.wifi.connects==1);assert(f.wifi.saved_ssid==std::string(32,'S'));
  f.service.poll();assert(f.wifi.connects==2);f.service.poll();assert(f.wifi.connects==2);assert(f.session.connected());assert(host_server.disconnects.empty());
}
void stale_during_query_hook(uint16_t handle, uint16_t mtu) {
  assert(handle==7);assert(mtu==23U);host_server.deliver_old_disconnect(handle);
}
void stale_during_query() {
  Fixture f;f.connect(7);const std::string command="CONNECT "+std::string(32,'S')+" "+std::string(64,'P');f.write(command,7);
  host_mtu_hook=stale_during_query_hook;f.service.poll();assert(f.wifi.connects==0);
  f.service.poll();assert(f.wifi.connects==1);assert(f.wifi.saved_ssid==std::string(32,'S'));
  assert(f.wifi.saved_password==std::string(64,'P'));assert(f.session.connected());
  assert(host_server.disconnects.empty());f.service.poll();assert(f.wifi.connects==1);
}
void stale_callbacks_live() {
  Fixture f;f.connect(7);f.write("CAL STATUS",7);
  for(int i=0;i<20;++i)host_server.deliver_old_disconnect(7);
  f.service.poll();assert(f.session.connected());assert(f.calibration.calls==1);
  assert(host_server.disconnects.empty());assert(f.calibration.disconnects==0);
}
void absent_without_callback() {
  Fixture f;f.connect(7);f.write("CAL STATUS",7);host_server.live_peers.clear();
  f.service.poll();assert(!f.session.connected());assert(f.calibration.calls==0);
  assert(f.calibration.disconnects==1);assert(f.wifi.cancels==1);
  f.service.poll();assert(f.calibration.disconnects==1);assert(f.wifi.cancels==1);
}
uint16_t reconnect_handle=8;
void absent_query_reconnect_hook(uint16_t handle, uint16_t mtu) {
  assert(handle==7);assert(mtu==0U);host_server.connect(reconnect_handle);host_server.service.characteristics[0]->write("CAL STATUS",reconnect_handle);
}
void absent_query_reconnect() {
  Fixture f;f.connect(7);host_server.live_peers.clear();host_mtu_hook=absent_query_reconnect_hook;
  f.service.poll();assert(f.session.connected());assert(f.calibration.calls==1);
  f.service.poll();assert(f.session.connected());assert(f.calibration.calls==1);
}
uint16_t same_reconnect_handle=7;
void absent_same_handle_reconnect_hook(uint16_t handle, uint16_t mtu) {
  assert(handle==7);assert(mtu==0U);host_server.connect(same_reconnect_handle);
  host_server.service.characteristics[0]->write("CAL STATUS",same_reconnect_handle);
}
void absent_same_handle_reconnect() {
  Fixture f;f.connect(7);host_server.live_peers.clear();same_reconnect_handle=7;
  host_mtu_hook=absent_same_handle_reconnect_hook;f.service.poll();
  assert(f.session.connected());assert(f.calibration.calls==1);
  assert(f.calibration.disconnects==1);assert(host_server.disconnects.empty());
}
void overflow_during_query_hook(uint16_t handle, uint16_t mtu) {
  assert(handle==7);assert(mtu==0U);
  for(int i=0;i<17;++i)host_server.service.characteristics[0]->write("CAL COMMIT TEST",handle);
}
void overflow_during_query() {
  Fixture f;f.connect(7);host_server.live_peers.clear();
  host_mtu_hook=overflow_during_query_hook;f.service.poll();
  assert(f.calibration.calls==0);assert(!f.session.connected());
  assert(host_server.disconnects==std::vector<uint16_t>{7});
  assert(f.calibration.disconnects==1);assert(f.wifi.cancels==1);
  f.service.poll();assert(host_server.disconnects.size()==1);
  assert(f.calibration.disconnects==1);assert(f.wifi.cancels==1);
  f.connect(8);f.write("CAL STATUS",8);f.service.poll();assert(f.calibration.calls==1);
}
void positive_mtu_overflow_during_query_hook(uint16_t handle, uint16_t mtu) {
  assert(handle==7);assert(mtu==23U);
  for(int i=0;i<17;++i)host_server.service.characteristics[0]->write("CAL COMMIT TEST",handle);
}
void positive_mtu_overflow_during_query() {
  Fixture f;f.connect(7);f.write("CAL STATUS",7);
  host_mtu_hook=positive_mtu_overflow_during_query_hook;f.service.poll();
  assert(f.calibration.calls==0);assert(!f.session.connected());
  assert(host_server.disconnects==std::vector<uint16_t>{7});
  assert(f.calibration.disconnects==1);assert(f.wifi.cancels==1);
  f.service.poll();assert(host_server.disconnects.size()==1);
  assert(f.calibration.disconnects==1);assert(f.wifi.cancels==1);
  f.connect(8);f.write("CAL STATUS",8);f.service.poll();assert(f.calibration.calls==1);
}
void handle_zero_presence() {
  Fixture f;f.connect(0);assert(f.session.connected());f.write("CAL STATUS",0);f.service.poll();
  assert(f.session.connected());assert(f.calibration.calls==1);
  host_server.peer_disconnect(0);f.service.poll();assert(!f.session.connected());
}
void overflow() {
  Fixture f;f.connect();for(int i=0;i<17;++i)f.write("CAL COMMIT TEST");assert(f.calibration.calls==0);
  f.service.poll();assert(f.calibration.calls==0);assert(host_server.disconnects==std::vector<uint16_t>{7});assert(!f.session.connected());
  for(int i=0;i<5;++i) { f.service.poll(); }
  assert(host_server.disconnects.size()==1);assert(f.wifi.cancels==1);
  f.connect(8);f.write("CAL STATUS",8);f.service.poll();assert(f.calibration.calls==1);
}
void timeout() {
  Fixture f;f.connect();f.write("F1|1|0|2|GET_");f.service.poll();host_now_us=20000;f.service.poll();
  assert(host_server.disconnects==std::vector<uint16_t>{7});assert(!f.session.connected());
  f.service.poll();assert(host_server.disconnects.size()==1);
  f.connect(8);f.write("F1|2|0|1|PING",8);f.service.poll();assert(f.session.connected());
  bool pong=false;for(const auto& n:f.response().notifications)if(n.handle==8&&n.bytes.find("PONG")!=std::string::npos)pong=true;assert(pong);
}
void long_write() {
  Fixture f;f.connect();const std::string command="CONNECT "+std::string(32,'S')+" "+std::string(64,'P');
  assert(command.size()==105);f.write(command);assert(f.wifi.connects==0);f.command().value.bytes.assign("PING");f.service.poll();
  assert(f.wifi.connects==1);assert(f.wifi.saved_ssid==std::string(32,'S'));assert(f.wifi.saved_password==std::string(64,'P'));
}
void oversized() {
  Fixture f;f.connect();f.write(std::string(193,'X'));f.service.poll();assert(host_server.disconnects==std::vector<uint16_t>{7});assert(f.calibration.calls==0);
}
void fault() {
  Fixture f;f.connect();f.write("CAL COMMIT TEST");host_fault=true;f.service.poll();assert(f.calibration.calls==0);
  assert(host_server.disconnects==std::vector<uint16_t>{7});assert(!f.session.connected());assert(host_server.advertising_stopped);assert(!host_server.advertise_on_disconnect);
  f.service.poll();assert(host_server.disconnects.size()==1);f.write("PING");assert(!f.session.connected());
}
struct ObservationLock final : sigap::debug::IDebugLock {
  void lock() noexcept override { assert(host_critical_depth==0); }
  void unlock() noexcept override { assert(host_critical_depth==0); }
};
std::vector<sigap::debug::EventRecord> observed_events(sigap::debug::DebugService& service,
                                                    uint16_t code) {
  std::vector<sigap::debug::EventRecord> result;
  const auto meta=service.meta(static_cast<uint64_t>(host_now_us));
  for (uint64_t cursor=meta.oldest>0U?meta.oldest-1U:0U;cursor<meta.latest;) {
    const auto page=service.read({meta.boot,cursor});
    assert(page.has_event);
    cursor=page.event.sequence;
    if (page.event.module==sigap::debug::Module::Ble && page.event.event_code==code)
      result.push_back(page.event);
  }
  return result;
}
void observed_commands(bool filtered=false, bool full=false) {
  Fixture f; ObservationLock lock;
  sigap::acquisition::BootId boot{};boot.present=true;boot.bytes[15]=1;
  sigap::debug::DebugService events(boot,"TEST",filtered?sigap::debug::Level::Error:sigap::debug::Level::Debug,lock);
  if (full) {
    sigap::debug::EventInput filler{};filler.module=sigap::debug::Module::Runtime;filler.event_code=1U;
    for (unsigned i=0U;i<64U;++i) assert(events.emit(filler));
  }
  f.session.bind_debug_sink(&events);
  const auto before_connect=events.meta(0U);
  host_server.connect(7);
  assert(events.meta(0U).latest==before_connect.latest);
  f.service.poll();
  assert(observed_events(events,10U).size()==(filtered?0U:1U));
  const auto before=events.meta(0U).latest;
  f.write("SCAN_WIFI");
  assert(f.wifi.scans==0 && events.meta(0U).latest==before);
  for (unsigned i=0U;i<32U;++i) f.service.poll();
  assert(f.wifi.scans==1 && f.session.pending_request_count()==0);
  const auto started=observed_events(events,12U);
  const auto completed=observed_events(events,14U);
  const auto progress=observed_events(events,13U);
  assert(!f.response().notifications.empty());
  if (filtered) {
    assert(started.empty() && completed.empty() && progress.empty());
    assert(events.meta(0U).latest==before_connect.latest);
  } else {
    assert(started.size()==1U && started[0].presence==0U);
    assert(completed.size()==2U && completed[0].arg0==1 && completed[1].arg0==0);
    assert(progress.size()==f.response().notifications.size());
  }
  if (full) assert(events.meta(0U).overwritten>before_connect.overwritten);
  for (const auto& event:progress) assert(event.arg0>0 && event.arg0<=event.arg1);
  const auto after=events.meta(0U).latest;
  f.write("F1|1|0|2|GET_DEB");
  for (unsigned i=0U;i<8U;++i) f.service.poll();
  assert(events.meta(0U).latest==after);
  f.write("F1|1|1|2|UG META");
  for (unsigned i=0U;i<32U;++i) f.service.poll();
  assert(events.meta(0U).latest==after && f.wifi.scans==1);
}
void observed_stale_request() {
  Fixture f; ObservationLock lock;
  sigap::acquisition::BootId boot{};boot.present=true;boot.bytes[15]=1;
  sigap::debug::DebugService events(boot,"TEST",sigap::debug::Level::Debug,lock);
  f.session.bind_debug_sink(&events);f.connect();
  f.write("SCAN_WIFI");
  host_server.peer_disconnect(7);
  f.service.poll();
  assert(f.wifi.scans==0 && f.wifi.cancels==1);
  assert(observed_events(events,12U).empty() && observed_events(events,13U).empty());
  assert(observed_events(events,14U).empty() && observed_events(events,15U).empty());
  const auto closed=observed_events(events,11U);
  assert(closed.size()==1U && closed[0].arg0==3 && closed[0].arg1==0);
  host_server.deliver_old_disconnect(7);f.service.poll();
  assert(observed_events(events,11U).size()==1U && f.response().notifications.empty());
}
void observed_notify_refusal() {
  Fixture f; ObservationLock lock;
  sigap::acquisition::BootId boot{};boot.present=true;boot.bytes[15]=1;
  sigap::debug::DebugService events(boot,"TEST",sigap::debug::Level::Debug,lock);
  f.session.bind_debug_sink(&events);f.connect();
  f.response().reject_notification_attempt=2U;
  f.write("LIST_KNOWN_WIFI");
  for (unsigned i=0U;i<32U;++i) f.service.poll();
  const auto progress=observed_events(events,13U),rejected=observed_events(events,15U);
  assert(progress.size()>1U && progress.front().arg0==1 && progress[1].arg0==2);
  assert(progress.back().arg0==progress.back().arg1-1);
  assert(progress.size()==f.response().notifications.size());
  assert(f.response().notification_attempts==f.response().notifications.size()+1U);
  assert(f.wifi.lists==1 && rejected.size()==1U);
  assert(rejected[0].arg0==2 && rejected[0].arg1==0);
}
void debug_commands() {
  struct Lock final : sigap::debug::IDebugLock {
    void lock() noexcept override { assert(host_critical_depth==0); }
    void unlock() noexcept override { assert(host_critical_depth==0); }
  } lock;
  Fixture f;host_now_us=INT64_C(9007199254740993);
  sigap::acquisition::BootId boot{};boot.present=true;boot.bytes[15]=1;
  sigap::debug::DebugService events(boot,"TEST",sigap::debug::Level::Info,lock);
  sigap::debug::CommandBackend backend(events);f.session.set_debug_backend(&backend);
  sigap::debug::EventInput event{};event.module=sigap::debug::Module::Runtime;event.event_code=1;event.arg0=1;
  assert(events.emit(event));event.monotonic_us=static_cast<uint64_t>(host_now_us);assert(events.emit(event));
  f.connect();f.write("GET_DEBUG META");assert(f.response().notifications.empty());
  for(int i=0;i<128;++i) { f.service.poll(); }
  std::string body;
  for(const auto& notification:f.response().notifications) {
    assert(notification.handle==7);assert(notification.bytes.size()<=20);
    size_t start=0;for(int i=0;i<4;++i){start=notification.bytes.find('|',start);assert(start!=std::string::npos);++start;}
    body+=notification.bytes.substr(start);
  }
  std::ifstream fixture("contracts/debug/v1/fixtures/ble.txt");std::string line,expected;
  while(std::getline(fixture,line))if(line.compare(0,5,"META=")==0)expected=line.substr(5);
  assert(!expected.empty());assert(body==expected);assert(f.calibration.calls==0);
  f.response().notifications.clear();f.write("SET_DEBUG_LEVEL DEBUG");
  assert(events.meta(static_cast<uint64_t>(host_now_us)).level==sigap::debug::Level::Info);
  f.service.poll();assert(events.meta(static_cast<uint64_t>(host_now_us)).level==sigap::debug::Level::Debug);assert(f.calibration.calls==0);
}
}
int main(int argc,char** argv) {
  assert(argc==2);const std::string_view name(argv[1]);
  if(name=="deferred")deferred();else if(name=="bounded")bounded();else if(name=="stale")stale();else if(name=="reused_handle")reused_handle();else if(name=="delayed_disconnect")delayed_disconnect();else if(name=="disconnect_during_query")disconnect_during_query();else if(name=="stale_during_query")stale_during_query();else if(name=="disconnect_between_writes")disconnect_between_writes();else if(name=="stale_hint_between_writes")stale_hint_between_writes();else if(name=="stale_callbacks_live")stale_callbacks_live();else if(name=="absent_without_callback")absent_without_callback();else if(name=="absent_query_reconnect")absent_query_reconnect();else if(name=="absent_same_handle_reconnect")absent_same_handle_reconnect();else if(name=="overflow_during_query")overflow_during_query();else if(name=="positive_mtu_overflow_during_query")positive_mtu_overflow_during_query();else if(name=="handle_zero_presence")handle_zero_presence();
  else if(name=="overflow")overflow();else if(name=="timeout")timeout();else if(name=="long_write")long_write();else if(name=="oversized")oversized();else if(name=="fault")fault();else if(name=="debug_commands")debug_commands();else if(name=="observed_commands")observed_commands();else if(name=="observed_notify_refusal")observed_notify_refusal();else if(name=="observed_filtered")observed_commands(true);else if(name=="observed_full")observed_commands(false,true);else if(name=="observed_stale")observed_stale_request();else return 2;
  std::cout<<"PASS "<<name<<"\n";
}
