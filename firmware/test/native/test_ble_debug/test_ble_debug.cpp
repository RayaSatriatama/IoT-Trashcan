#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "debug/commands.h"
#include "debug/service.h"
#include "provisioning/ble_provisioning.h"
#include "unity.h"

using namespace sigap;
using namespace sigap::provisioning;

namespace {
struct Clock final : BleProvisioningClock { uint64_t now{}; uint64_t now_us() const noexcept override { return now; } };
struct Wifi final : IBleProvisioningBackend {
  WifiScanStatus requested{WifiScanStatus::Pending}; int scan_calls{},cancel_calls{};
  WifiScanStatus polled{WifiScanStatus::Ready};
  WifiScanStatus listed{WifiScanStatus::Ready};
  bool accepted{true};
  unsigned connects{}, retries{};
  size_t listed_count{};
  std::function<void()> before_action;
  WifiState wifi_state() const noexcept override { return WifiState::Connected; }
  std::string_view current_ssid() const noexcept override { return "test-net"; }
  WifiScanStatus request_scan() noexcept override { if (before_action) before_action(); ++scan_calls; return requested; }
  WifiScanStatus poll_scan(WifiNetwork*,size_t,size_t& count) noexcept override { count=0; return polled; }
  WifiScanStatus list_known_wifi(WifiNetwork* output,size_t capacity,size_t& count) noexcept override {
    count=std::min(listed_count,capacity);
    for (size_t i=0U;i<count;++i) { output[i].ssid.fill('N'); output[i].ssid.back()='\0'; output[i].secure=true; }
    return listed;
  }
  bool request_connect(std::string_view,std::string_view) noexcept override { if (before_action) before_action(); ++connects; return accepted; }
  bool retry_wifi() noexcept override { if (before_action) before_action(); ++retries; return accepted; }
  void cancel() noexcept override { ++cancel_calls; }
};
struct Calibration final : IBleCalibrationBackend {
  int executes{},disconnects{}; std::string last;
  bool execute(std::string_view command,char* output,size_t capacity,size_t& size) noexcept override {
    ++executes;last.assign(command);constexpr std::string_view reply="OK CAL";if(capacity<reply.size())return false;
    std::memcpy(output,reply.data(),reply.size());size=reply.size();return true;
  }
  void disconnect() noexcept override {++disconnects;}
};
struct Lock final : debug::IDebugLock { void lock() noexcept override {} void unlock() noexcept override {} };
struct Frames { std::string body; unsigned count{}; bool valid{false}; };
Frames drain(BleProvisioningSession& session, BleResponse& response, uint32_t id=0U) {
  Frames result;
  unsigned expected = 0U;
  while (result.count < 128U && response.notify) {
    const std::string_view packet(response.bytes.data(), response.size);
    if (packet.size() > 20U || packet.substr(0U, 3U) != "R1|") return result;
    size_t position = 3U;
    uint32_t fields[3]{};
    for (auto& field : fields) {
      const auto end = packet.find('|', position);
      if (end == packet.npos || end == position) return result;
      const auto parsed = std::from_chars(packet.data()+position, packet.data()+end, field);
      if (parsed.ec != std::errc{} || parsed.ptr != packet.data()+end) return result;
      position = end+1U;
    }
    if (fields[0] != id || fields[2] == 0U || fields[2] > 128U ||
        fields[1] != result.count || position == packet.size()) return result;
    if (result.count == 0U) expected = fields[2];
    if (fields[2] != expected) return result;
    result.body.append(packet.substr(position));
    if (++result.count == expected) {
      result.valid = true;
      return result;
    }
    session.poll(response);
  }
  return result;
}

bool send(BleProvisioningSession& session, uint32_t id, std::string_view command,
          BleResponse& response) {
  const size_t count = (command.size()+1U)/2U;
  for (size_t index=0U; index<count; ++index) {
    const auto packet = "F1|"+std::to_string(id)+"|"+std::to_string(index)+"|"+
        std::to_string(count)+"|"+std::string(command.substr(index*2U, 2U));
    if (packet.size() > 20U || session.receive(packet, response) != BleParseStatus::Accepted)
      return false;
    if (index+1U < count) {
      const auto acknowledgement = drain(session, response, id);
      if (!acknowledgement.valid || acknowledgement.body != "OK CHUNK") return false;
    }
  }
  return count != 0U;
}

std::string fixture(std::string_view name) {
  std::ifstream input("../contracts/debug/v1/fixtures/ble.txt");
  std::string line;
  while (std::getline(input, line)) {
    if (line.compare(0U, name.size(), name) == 0 && line[name.size()] == '=')
      return line.substr(name.size()+1U);
  }
  return {};
}
void test_debug_backend_dispatch_is_real_typed_and_noncalibration() {
  Wifi wifi;Clock clock;Calibration calibration;Lock lock;acquisition::BootId boot{};boot.present=true;boot.bytes[15]=1;
  clock.now=UINT64_C(9007199254740993);
  debug::DebugService service(boot,"TEST",debug::Level::Info,lock);debug::CommandBackend backend(service);
  debug::EventInput event{};event.module=debug::Module::Runtime;event.event_code=1;event.arg0=1;
  TEST_ASSERT_TRUE(service.emit(event));event.monotonic_us=clock.now;TEST_ASSERT_TRUE(service.emit(event));
  BleProvisioningSession session(wifi,clock);session.set_debug_backend(&backend);session.set_calibration_backend(&calibration);session.on_connect(clock.now);
  BleResponse response{};TEST_ASSERT_TRUE(send(session,9U,"GET_DEBUG META",response));
  auto meta=drain(session,response,9U);TEST_ASSERT_TRUE(meta.valid);
  const auto expected_meta=fixture("META");TEST_ASSERT_FALSE(expected_meta.empty());
  TEST_ASSERT_EQUAL_STRING(expected_meta.c_str(),meta.body.c_str());TEST_ASSERT_EQUAL_INT(0,calibration.executes);
  TEST_ASSERT_TRUE(send(session,10U,"GET_DEBUG PAGE 00000000000000000000000000000001 1",response));
  auto page=drain(session,response,10U);TEST_ASSERT_TRUE(page.valid);
  const auto expected_page=fixture("PAGE");TEST_ASSERT_FALSE(expected_page.empty());
  TEST_ASSERT_EQUAL_STRING(expected_page.c_str(),page.body.c_str());
  TEST_ASSERT_TRUE(send(session,11U,"SET_DEBUG_LEVEL DEBUG",response));auto level=drain(session,response,11U);TEST_ASSERT_TRUE(level.valid);TEST_ASSERT_EQUAL_STRING("DBG1|L|3",level.body.c_str());
  TEST_ASSERT_EQUAL_INT(0,calibration.executes);
}
void test_debug_unavailable_invalid_grammar_and_u64_boundaries() {
  Wifi wifi;Clock clock;BleProvisioningSession session(wifi,clock);session.on_connect(0);BleCommand command{};
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,BleCommandParser::parse("GET_DEBUG PAGE 0123456789abcdef0123456789abcdef 18446744073709551615",command));
  TEST_ASSERT_EQUAL_INT(BleCommandKind::DebugPage,command.kind);TEST_ASSERT_EQUAL_UINT64(UINT64_MAX,command.debug_cursor.after);
  for(const char* input:{"GET_DEBUG META extra","GET_DEBUG PAGE 0123456789ABCDEF0123456789abcdef 0","GET_DEBUG PAGE 0123456789abcdef0123456789abcdef 01","GET_DEBUG PAGE 0123456789abcdef0123456789abcdef 18446744073709551616","GET_DEBUG PAGE 0123456789abcdef0123456789abcdef +1","SET_DEBUG_LEVEL TRACE","SET_DEBUG_LEVEL INFO extra"})
    TEST_ASSERT_EQUAL_INT(BleParseStatus::Invalid,BleCommandParser::parse(input,command));
  BleResponse response{};TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,session.receive("GET_DEBUG META",response));
  auto unavailable=drain(session,response);TEST_ASSERT_TRUE(unavailable.valid);TEST_ASSERT_EQUAL_STRING("ERR DEBUG_UNAVAILABLE",unavailable.body.c_str());
}
void test_timeout_disconnect_signal_is_one_shot_and_reset_on_connect() {
  Wifi wifi;Clock clock;ProvisioningPolicy policy{};policy.session_timeout_ms=10;BleProvisioningSession session(wifi,clock,policy);session.on_connect(0);
  BleResponse response{};clock.now=10000;session.poll(response);TEST_ASSERT_FALSE(session.connected());TEST_ASSERT_TRUE(session.consume_disconnect_request());
  TEST_ASSERT_FALSE(session.consume_disconnect_request());session.on_connect(clock.now);TEST_ASSERT_FALSE(session.consume_disconnect_request());
}
void test_scan_start_preserves_rate_limit_and_failure_class() {
  Wifi wifi;Clock clock;BleProvisioningSession session(wifi,clock);session.on_connect(0);BleResponse response{};wifi.requested=WifiScanStatus::RateLimited;
  session.receive("SCAN_WIFI",response);auto rate=drain(session,response);TEST_ASSERT_TRUE(rate.valid);TEST_ASSERT_EQUAL_STRING("ERR SCAN_RATE_LIMITED",rate.body.c_str());
  wifi.requested=WifiScanStatus::Failed;session.receive("SCAN_WIFI",response);auto failed=drain(session,response);TEST_ASSERT_TRUE(failed.valid);TEST_ASSERT_EQUAL_STRING("ERR SCAN_FAILED",failed.body.c_str());
  wifi.requested=WifiScanStatus::Pending;session.receive("SCAN_WIFI",response);auto pending=drain(session,response);TEST_ASSERT_TRUE(pending.valid);TEST_ASSERT_EQUAL_STRING("OK SCAN PENDING",pending.body.c_str());
  session.poll(response);auto ready=drain(session,response);TEST_ASSERT_TRUE(ready.valid);TEST_ASSERT_EQUAL_STRING("OK SCAN READY count=00",ready.body.c_str());
}
}
void test_empty_page_cursor_and_present_zero_fields_survive_session() {
  Wifi wifi;Clock clock;Lock lock;acquisition::BootId boot{};
  boot.present=true;boot.bytes[15]=1;clock.now=UINT64_C(9007199254740993);
  debug::DebugService service(boot,"TEST",debug::Level::Info,lock);
  debug::CommandBackend backend(service);BleProvisioningSession session(wifi,clock);
  session.set_debug_backend(&backend);session.on_connect(clock.now);
  BleResponse response{};
  TEST_ASSERT_TRUE(send(session,9U,"GET_DEBUG PAGE 00000000000000000000000000000001 0",response));
  auto empty=drain(session,response,9U);TEST_ASSERT_TRUE(empty.valid);
  const auto expected=fixture("EMPTY_PAGE");TEST_ASSERT_FALSE(expected.empty());
  TEST_ASSERT_EQUAL_STRING(expected.c_str(),empty.body.c_str());
  TEST_ASSERT_TRUE(send(session,10U,"GET_DEBUG PAGE 00000000000000000000000000000001 18446744073709551615",response));
  auto cursor=drain(session,response,10U);TEST_ASSERT_TRUE(cursor.valid);
  TEST_ASSERT_EQUAL_STRING("DBG1|P|00000000000000000000000000000001|2|0|18446744073709551615|0|0|0|-",cursor.body.c_str());
  debug::EventInput event{};event.module=debug::Module::Runtime;event.event_code=1;
  event.monotonic_us=clock.now;event.arg0=1;event.presence=debug::kHasRequestId|debug::kHasRelatedSequence;
  TEST_ASSERT_TRUE(service.emit(event));
  TEST_ASSERT_TRUE(send(session,11U,"GET_DEBUG PAGE 00000000000000000000000000000001 0",response));
  auto zeros=drain(session,response,11U);TEST_ASSERT_TRUE(zeros.valid);
  TEST_ASSERT_EQUAL_STRING("DBG1|P|00000000000000000000000000000001|2|1|1|0|0|0|1,9007199254740993,2,1,1,0,0,1,0,0",zeros.body.c_str());
}

void test_max_request_id_returns_bounded_error_for_unframeable_meta() {
  Wifi wifi;Clock clock;Lock lock;acquisition::BootId boot{};
  boot.present=true;boot.bytes[15]=1;
  debug::DebugService service(boot,std::string(96U,'X'),debug::Level::Info,lock);
  debug::CommandBackend backend(service);BleProvisioningSession session(wifi,clock);
  session.set_debug_backend(&backend);session.on_connect(0U);
  BleResponse response{};
  TEST_ASSERT_TRUE(send(session,UINT32_MAX,"GET_DEBUG META",response));
  auto result=drain(session,response,UINT32_MAX);
  TEST_ASSERT_TRUE_MESSAGE(result.valid,"long-ID response must consist of complete <=20-byte R1 packets");
  TEST_ASSERT_EQUAL_STRING("ERR OVERSIZED",result.body.c_str());
  TEST_ASSERT_TRUE(session.connected());
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,session.receive("PING",response));
  auto ping=drain(session,response);TEST_ASSERT_TRUE(ping.valid);
  TEST_ASSERT_EQUAL_STRING("OK PONG",ping.body.c_str());
  session.on_disconnect();session.on_connect(0U);
  TEST_ASSERT_TRUE(send(session,9U,"GET_DEBUG META",response));
  auto normal=drain(session,response,9U);TEST_ASSERT_TRUE(normal.valid);
  TEST_ASSERT_TRUE(normal.body.rfind("DBG1|M|",0U)==0U);
}

struct ObservedSession {
  Wifi wifi;
  Clock clock;
  Lock lock;
  acquisition::BootId boot = [] { acquisition::BootId value{}; value.present=true; value.bytes[15]=1; return value; }();
  debug::DebugService service{boot, "TEST", debug::Level::Debug, lock};
  debug::CommandBackend commands{service};
  BleProvisioningSession session;
  explicit ObservedSession(ProvisioningPolicy policy = kDefaultProvisioningPolicy)
      : session(wifi, clock, policy) {
    session.bind_debug_sink(&service);
    session.set_debug_backend(&commands);
    session.on_connect(0U);
  }
  std::vector<debug::EventRecord> events(uint16_t code, uint64_t after=0U) {
    std::vector<debug::EventRecord> result;
    const auto meta=service.meta(clock.now);
    while (after<meta.latest) {
      const auto page=service.read({meta.boot,after});
      if (!page.has_event) break;
      after=page.event.sequence;
      if (page.event.module==debug::Module::Ble && page.event.event_code==code)
        result.push_back(page.event);
    }
    return result;
  }
};

void test_observed_link_and_disconnect_count_are_exact_before_cleanup() {
  ObservedSession f;
  TEST_ASSERT_EQUAL_UINT(1U,f.events(10U).size());
  f.session.on_connect(0U);
  TEST_ASSERT_EQUAL_UINT(1U,f.events(10U).size());
  BleResponse response{};
  TEST_ASSERT_TRUE(send(f.session,0U,"SCAN_WIFI",response));
  TEST_ASSERT_TRUE(drain(f.session,response,0U).valid);
  TEST_ASSERT_EQUAL_INT32(1,f.session.pending_request_count());
  f.session.on_disconnect();
  const auto closed=f.events(11U);
  TEST_ASSERT_EQUAL_UINT(1U,closed.size());
  TEST_ASSERT_EQUAL_INT32(3,closed[0].arg0);
  TEST_ASSERT_EQUAL_INT32(1,closed[0].arg1);
  TEST_ASSERT_EQUAL_UINT8(0U,closed[0].presence);
  TEST_ASSERT_EQUAL_INT32(0,f.session.pending_request_count());
  f.session.on_disconnect();
  TEST_ASSERT_EQUAL_UINT(1U,f.events(11U).size());
  TEST_ASSERT_EQUAL_INT(2,f.wifi.cancel_calls);
}

void test_observed_idle_expiry_is_once_and_uses_precleanup_count_and_saturation() {
  ProvisioningPolicy policy{}; policy.session_timeout_ms=1U;
  ObservedSession f(policy); BleResponse response{};
  f.session.receive("SCAN_WIFI",response);
  TEST_ASSERT_TRUE(drain(f.session,response).valid);
  f.clock.now=uint64_t(INT32_MAX)+1U;
  f.session.poll(response);
  const auto idle=f.events(16U),closed=f.events(11U);
  TEST_ASSERT_EQUAL_UINT(1U,idle.size());
  TEST_ASSERT_EQUAL_INT32(INT32_MAX,idle[0].arg0);
  TEST_ASSERT_EQUAL_INT32(1,idle[0].arg1);
  TEST_ASSERT_EQUAL_UINT(1U,closed.size());
  TEST_ASSERT_EQUAL_INT32(1,closed[0].arg0);
  TEST_ASSERT_EQUAL_INT32(1,closed[0].arg1);
  TEST_ASSERT_TRUE(f.session.consume_disconnect_request());
  f.session.poll(response);
  TEST_ASSERT_EQUAL_UINT(1U,f.events(16U).size());
  TEST_ASSERT_FALSE(f.session.consume_disconnect_request());
  f.session.on_connect(f.clock.now);
  f.clock.now=0U;
  f.session.poll(response);
  TEST_ASSERT_TRUE(f.session.connected());
  TEST_ASSERT_EQUAL_UINT(1U,f.events(16U).size());
}

void test_observed_scan_status_matrix_preserves_id_zero_and_logical_byte_units() {
  for (const auto status:{WifiScanStatus::Pending,WifiScanStatus::Ready,
                         WifiScanStatus::RateLimited,WifiScanStatus::Failed,WifiScanStatus::Idle}) {
    ObservedSession f; f.wifi.requested=status;
    bool start_visible{};
    f.wifi.before_action=[&] { start_visible=f.events(12U).size()==1U; };
    BleResponse response{};
    TEST_ASSERT_TRUE(send(f.session,0U,"SCAN_WIFI",response));
    const auto body=drain(f.session,response,0U);
    TEST_ASSERT_TRUE(body.valid);
    TEST_ASSERT_TRUE(start_visible);
    const auto started=f.events(12U),completed=f.events(14U),rejected=f.events(15U);
    TEST_ASSERT_EQUAL_UINT(1U,started.size());
    TEST_ASSERT_EQUAL_INT32(1,started[0].arg0);
    TEST_ASSERT_EQUAL_INT32(std::string_view("SCAN_WIFI").size(),started[0].arg1);
    TEST_ASSERT_EQUAL_UINT8(debug::kHasRequestId,started[0].presence);
    TEST_ASSERT_EQUAL_UINT32(0U,started[0].request_id);
    TEST_ASSERT_EQUAL_UINT(0U,f.events(13U).size());
    if (status==WifiScanStatus::Ready || status==WifiScanStatus::Pending) {
      TEST_ASSERT_EQUAL_UINT(1U,completed.size());
      TEST_ASSERT_EQUAL_INT32(status==WifiScanStatus::Pending?1:0,completed[0].arg0);
      TEST_ASSERT_EQUAL_INT32(body.body.size(),completed[0].arg1);
      TEST_ASSERT_EQUAL_UINT(0U,rejected.size());
    } else {
      TEST_ASSERT_EQUAL_UINT(0U,completed.size());
      TEST_ASSERT_EQUAL_UINT(1U,rejected.size());
      TEST_ASSERT_EQUAL_INT32(0,rejected[0].arg0);
      TEST_ASSERT_EQUAL_INT32(0,rejected[0].arg1);
    }
    TEST_ASSERT_EQUAL_INT(1,f.wifi.scan_calls);
  }
}

void test_observed_boolean_rejection_is_not_parser_acceptance_or_wifi_success() {
  for (const auto text:{"CONNECT lab password","RETRY_WIFI"}) {
    ObservedSession f; f.wifi.accepted=false; BleResponse response{};
    TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,f.session.receive(text,response));
    TEST_ASSERT_TRUE(drain(f.session,response).valid);
    TEST_ASSERT_EQUAL_UINT(1U,f.events(12U).size());
    TEST_ASSERT_EQUAL_UINT8(0U,f.events(12U)[0].presence);
    TEST_ASSERT_EQUAL_UINT(0U,f.events(14U).size());
    const auto rejected=f.events(15U);
    TEST_ASSERT_EQUAL_UINT(1U,rejected.size());
    TEST_ASSERT_EQUAL_INT32(0,rejected[0].arg0);
    TEST_ASSERT_EQUAL_INT32(0,rejected[0].arg1);
    TEST_ASSERT_EQUAL_UINT(1U,f.wifi.connects+f.wifi.retries);
  }
}

void test_observed_pending_scan_supersession_and_routine_reads_do_not_steal_identity() {
  ObservedSession f; BleResponse response{};
  for (uint32_t id=0U;id<2U;++id) {
    TEST_ASSERT_TRUE(send(f.session,id,"SCAN_WIFI",response));
    TEST_ASSERT_TRUE(drain(f.session,response,id).valid);
    TEST_ASSERT_EQUAL_INT32(1,f.session.pending_request_count());
  }
  const auto before=f.service.meta(0U).latest;
  TEST_ASSERT_TRUE(send(f.session,2U,"GET_DEBUG META",response));
  TEST_ASSERT_TRUE(drain(f.session,response,2U).valid);
  TEST_ASSERT_EQUAL_UINT64(before,f.service.meta(0U).latest);
  f.wifi.polled=WifiScanStatus::Pending;
  f.session.poll(response);
  TEST_ASSERT_FALSE(response.notify);
  TEST_ASSERT_EQUAL_UINT64(before,f.service.meta(0U).latest);
  f.wifi.polled=WifiScanStatus::Failed;
  f.session.poll(response);
  TEST_ASSERT_TRUE(drain(f.session,response,1U).valid);
  const auto rejected=f.events(15U,before);
  TEST_ASSERT_EQUAL_UINT(1U,rejected.size());
  TEST_ASSERT_EQUAL_UINT32(1U,rejected[0].request_id);
  TEST_ASSERT_EQUAL_UINT8(debug::kHasRequestId,rejected[0].presence);
  TEST_ASSERT_EQUAL_INT32(0,f.session.pending_request_count());
  f.session.poll(response);
  TEST_ASSERT_EQUAL_UINT(1U,f.events(15U,before).size());
}

void test_observed_debug_partial_ack_and_routine_commands_are_excluded() {
  ObservedSession f; BleResponse response{};
  const auto before=f.service.meta(0U).latest;
  uint32_t id=0U;
  for (const auto text:{"GET_DEBUG META","GET_DEBUG PAGE 00000000000000000000000000000001 0",
                        "SET_DEBUG_LEVEL DEBUG","GET_STATUS","GET_DEVICE_ID","PING","CAL STATUS"}) {
    TEST_ASSERT_TRUE(send(f.session,id,text,response));
    TEST_ASSERT_TRUE(drain(f.session,response,id).valid);
    ++id;
    TEST_ASSERT_EQUAL_UINT64(before,f.service.meta(0U).latest);
  }
  f.session.receive("INVALID",response);
  TEST_ASSERT_TRUE(drain(f.session,response).valid);
  const auto rejected=f.events(15U,before);
  TEST_ASSERT_EQUAL_UINT(1U,rejected.size());
  TEST_ASSERT_EQUAL_INT32(1,rejected[0].arg0);
  TEST_ASSERT_EQUAL_INT32(3,rejected[0].arg1);
}

void test_observed_sink_modes_preserve_business_without_retry_or_render_progress() {
  struct Refusing final : debug::IEventSink {
    unsigned calls{};
    bool emit(const debug::EventInput&) noexcept override { ++calls; return false; }
  };
  for (unsigned mode=0U;mode<5U;++mode) {
    ObservedSession f; Refusing refusing;
    if (mode==0U) f.session.bind_debug_sink(nullptr);
    if (mode==1U) f.session.bind_debug_sink(&refusing);
    if (mode==2U) TEST_ASSERT_TRUE(f.service.set_level(debug::Level::Error));
    if (mode==3U) {
      debug::EventInput filler{}; filler.module=debug::Module::Runtime; filler.event_code=1U;
      for (unsigned i=0U;i<64U;++i) TEST_ASSERT_TRUE(f.service.emit(filler));
    }
    const auto before=f.service.meta(0U);
    f.wifi.requested=WifiScanStatus::Ready;
    BleResponse response{};
    TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,f.session.receive("SCAN_WIFI",response));
    const auto body=drain(f.session,response);
    TEST_ASSERT_TRUE(body.valid);
    TEST_ASSERT_EQUAL_STRING("OK SCAN READY",body.body.c_str());
    TEST_ASSERT_EQUAL_INT(1,f.wifi.scan_calls);
    TEST_ASSERT_EQUAL_INT32(0,f.session.pending_request_count());
    f.session.poll(response);
    TEST_ASSERT_FALSE(response.notify);
    TEST_ASSERT_EQUAL_UINT(0U,f.events(13U).size());
    const auto after=f.service.meta(0U);
    if (mode<3U) TEST_ASSERT_EQUAL_UINT64(before.latest,after.latest);
    else TEST_ASSERT_EQUAL_UINT64(before.latest+2U,after.latest);
    if (mode==3U) TEST_ASSERT_EQUAL_UINT64(before.overwritten+2U,after.overwritten);
    if (mode==1U) TEST_ASSERT_EQUAL_UINT(2U,refusing.calls);
    f.session.on_disconnect();
    TEST_ASSERT_EQUAL_INT(1,f.wifi.cancel_calls);
    if (mode==1U) TEST_ASSERT_EQUAL_UINT(3U,refusing.calls);
  }
}

void test_observed_duration_uses_real_clock_and_async_saturates_u32() {
  ObservedSession sync; BleResponse response{};
  sync.wifi.before_action=[&] { sync.clock.now+=25U; };
  sync.session.receive("RETRY_WIFI",response);
  TEST_ASSERT_TRUE(drain(sync.session,response).valid);
  const auto completed=sync.events(14U);
  TEST_ASSERT_EQUAL_UINT(1U,completed.size());
  TEST_ASSERT_EQUAL_UINT32(25U,completed[0].duration_us);
  TEST_ASSERT_EQUAL_UINT64(25U,completed[0].monotonic_us);
  ProvisioningPolicy policy{}; policy.session_timeout_ms=UINT32_MAX;
  ObservedSession async(policy);
  TEST_ASSERT_TRUE(send(async.session,0U,"SCAN_WIFI",response));
  TEST_ASSERT_TRUE(drain(async.session,response,0U).valid);
  async.clock.now=uint64_t(UINT32_MAX)+1U;
  async.session.poll(response);
  TEST_ASSERT_TRUE(drain(async.session,response,0U).valid);
  const auto terminal=async.events(14U);
  TEST_ASSERT_EQUAL_UINT(2U,terminal.size());
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX,terminal.back().duration_us);
  TEST_ASSERT_EQUAL_UINT32(0U,terminal.back().request_id);
  TEST_ASSERT_EQUAL_UINT8(debug::kHasRequestId,terminal.back().presence);
}

void test_observed_list_status_and_geometry_failure_never_claim_success() {
  for (const auto status:{WifiScanStatus::Ready,WifiScanStatus::Pending,
                         WifiScanStatus::RateLimited,WifiScanStatus::Failed,WifiScanStatus::Idle}) {
    ObservedSession f; f.wifi.listed=status; BleResponse response{};
    TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,f.session.receive("LIST_KNOWN_WIFI",response));
    const auto body=drain(f.session,response);
    TEST_ASSERT_TRUE(body.valid);
    TEST_ASSERT_EQUAL_UINT(status==WifiScanStatus::Ready?1U:0U,f.events(14U).size());
    TEST_ASSERT_EQUAL_UINT(status==WifiScanStatus::Ready?0U:1U,f.events(15U).size());
    TEST_ASSERT_EQUAL_INT32(0,f.session.pending_request_count());
    if (status==WifiScanStatus::Ready)
      TEST_ASSERT_EQUAL_INT32(body.body.size(),f.events(14U)[0].arg1);
  }
  ObservedSession f; f.wifi.listed_count=16U; BleResponse response{};
  TEST_ASSERT_TRUE(send(f.session,UINT32_MAX,"LIST_KNOWN_WIFI",response));
  const auto body=drain(f.session,response,UINT32_MAX);
  TEST_ASSERT_TRUE(body.valid);
  TEST_ASSERT_EQUAL_STRING("ERR OVERSIZED",body.body.c_str());
  TEST_ASSERT_EQUAL_UINT(0U,f.events(14U).size());
  const auto rejected=f.events(15U);
  TEST_ASSERT_EQUAL_UINT(1U,rejected.size());
  TEST_ASSERT_EQUAL_INT32(5,rejected[0].arg1);
}

void test_observed_diagnostic_readonly_denies_before_start_or_backend_call() {
  ObservedSession f; f.session.set_read_only_diagnostic(true); BleResponse response{};
  const auto before=f.service.meta(0U).latest;
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Invalid,f.session.receive("SCAN_WIFI",response));
  TEST_ASSERT_TRUE(drain(f.session,response).valid);
  TEST_ASSERT_EQUAL_INT(0,f.wifi.scan_calls);
  TEST_ASSERT_EQUAL_UINT64(before,f.service.meta(0U).latest);
}

void test_failed_pending_scan_keeps_id_after_unrelated_status_request() {
  Wifi wifi;
  Clock clock;
  BleProvisioningSession session(wifi, clock);
  session.on_connect(0U);
  BleResponse response{};
  TEST_ASSERT_TRUE(send(session, 0U, "SCAN_WIFI", response));
  const auto pending = drain(session, response, 0U);
  TEST_ASSERT_TRUE(pending.valid);
  TEST_ASSERT_EQUAL_STRING("OK SCAN PENDING", pending.body.c_str());
  TEST_ASSERT_TRUE(send(session, 1U, "GET_STATUS", response));
  TEST_ASSERT_TRUE(drain(session, response, 1U).valid);
  wifi.polled = WifiScanStatus::Failed;
  session.poll(response);
  const auto failed = drain(session, response, 0U);
  TEST_ASSERT_TRUE_MESSAGE(failed.valid, "terminal scan failure must retain the scan request ID");
  TEST_ASSERT_EQUAL_STRING("ERR SCAN_FAILED", failed.body.c_str());
  TEST_ASSERT_EQUAL_INT(1, wifi.scan_calls);
  session.poll(response);
  TEST_ASSERT_FALSE(response.notify);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_debug_backend_dispatch_is_real_typed_and_noncalibration);
  RUN_TEST(test_debug_unavailable_invalid_grammar_and_u64_boundaries);
  RUN_TEST(test_timeout_disconnect_signal_is_one_shot_and_reset_on_connect);
  RUN_TEST(test_scan_start_preserves_rate_limit_and_failure_class);
  RUN_TEST(test_empty_page_cursor_and_present_zero_fields_survive_session);
  RUN_TEST(test_max_request_id_returns_bounded_error_for_unframeable_meta);
  RUN_TEST(test_failed_pending_scan_keeps_id_after_unrelated_status_request);
  RUN_TEST(test_observed_link_and_disconnect_count_are_exact_before_cleanup);
  RUN_TEST(test_observed_idle_expiry_is_once_and_uses_precleanup_count_and_saturation);
  RUN_TEST(test_observed_scan_status_matrix_preserves_id_zero_and_logical_byte_units);
  RUN_TEST(test_observed_boolean_rejection_is_not_parser_acceptance_or_wifi_success);
  RUN_TEST(test_observed_pending_scan_supersession_and_routine_reads_do_not_steal_identity);
  RUN_TEST(test_observed_debug_partial_ack_and_routine_commands_are_excluded);
  RUN_TEST(test_observed_sink_modes_preserve_business_without_retry_or_render_progress);
  RUN_TEST(test_observed_duration_uses_real_clock_and_async_saturates_u32);
  RUN_TEST(test_observed_list_status_and_geometry_failure_never_claim_success);
  RUN_TEST(test_observed_diagnostic_readonly_denies_before_start_or_backend_call);
  return UNITY_END();
}
