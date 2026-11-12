#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <unity.h>
#include "debug/service.h"
#include "debug/usb_sink.h"
#include "debug/commands.h"
#include "debug/service.cpp"
#include "debug/codec.cpp"
#include "debug/usb_sink.cpp"
#include "debug/commands.cpp"
#include "maintenance/maintenance.h"
#include "maintenance/maintenance.cpp"

namespace {
using namespace sigap;
struct Lock final : sigap::debug::IDebugLock {
  void lock() noexcept override { ++depth; }
  void unlock() noexcept override { --depth; }
  unsigned depth{0};
};
struct Writer final : sigap::debug::IUsbDebugWriter {
  size_t writable() noexcept override { return allowance; }
  size_t write(const char* bytes, size_t count) noexcept override {
    if (lock != nullptr && lock->depth != 0U) ++writes_under_lock;
    if (zero) return 0U;
    count = std::min(count, max_write);
    output.append(bytes, count);
    return count;
  }
  Lock* lock{nullptr};
  unsigned writes_under_lock{0U};
  size_t allowance{384U}, max_write{384U};
  bool zero{false};
  std::string output;
};
void test_partial_zero_and_budgeted_usb_delivery() {
  acquisition::BootId boot{}; boot.present=true; boot.bytes[15]=1U;
  Lock lock;
  sigap::debug::DebugService service(boot,"TEST",sigap::debug::Level::Info,lock);
  Writer writer{};
  writer.lock=&lock;
  writer.max_write=2U;
  sigap::debug::UsbDebugSink sink(service,writer);
  sigap::debug::EventInput event{}; event.module=sigap::debug::Module::Runtime;
  event.event_code=1U; event.level=sigap::debug::Level::Info;
  TEST_ASSERT_TRUE(service.emit(event));
  sink.poll(0U,1U);
  TEST_ASSERT_EQUAL_UINT32(1U, writer.output.size());
  TEST_ASSERT_EQUAL_INT('S', writer.output[0]);
  for (unsigned i=0;i<1000U && writer.output.find('\n')==std::string::npos;++i) sink.poll(0U,3U);
  TEST_ASSERT_TRUE(writer.output.rfind("SIGAP_DEBUG ",0U)==0U);
  TEST_ASSERT_EQUAL_INT('\n',writer.output.back());
  TEST_ASSERT_EQUAL_UINT32(0U,writer.writes_under_lock);
}
void test_zero_write_does_not_spin_or_lose_cursor() {
  acquisition::BootId boot{}; boot.present=true; boot.bytes[15]=1U;
  Lock lock;
  sigap::debug::DebugService service(boot,"TEST",sigap::debug::Level::Info,lock);
  Writer writer{}; writer.zero=true;
  sigap::debug::UsbDebugSink sink(service,writer);
  sigap::debug::EventInput event{}; event.module=sigap::debug::Module::Runtime; event.event_code=1U;
  TEST_ASSERT_TRUE(service.emit(event));
  writer.allowance=0U;
  sink.poll(0U,384U);
  TEST_ASSERT_TRUE(writer.output.empty());
  writer.allowance=384U;
  sink.poll(0U,384U);
  TEST_ASSERT_TRUE(writer.output.empty());
  TEST_ASSERT_EQUAL_UINT64(0U,service.meta(0U).usb_lost);
}
void test_parser_accepts_debug_grammar_and_rejects_noncanonical_cursor() {
  maintenance::MaintenanceParser parser{};
  maintenance::ParseEvent event{};
  auto parse = [&](const char* line) {
    bool emitted = false;
    for (const char c : std::string(line)) emitted = parser.consume(c, event) || emitted;
    return emitted;
  };
  TEST_ASSERT_TRUE(parse("debug meta\n"));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(maintenance::CommandKind::DebugMeta), static_cast<int>(event.command.kind));
  TEST_ASSERT_TRUE(parse("debug page 00000000000000000000000000000001 18446744073709551615\n"));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(maintenance::CommandKind::DebugPage), static_cast<int>(event.command.kind));
  TEST_ASSERT_EQUAL_STRING("18446744073709551615", event.command.value.c_str());
  TEST_ASSERT_TRUE(parse("debug level DEBUG\n"));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(maintenance::CommandKind::DebugLevel), static_cast<int>(event.command.kind));
  TEST_ASSERT_TRUE(parse("debug page 00000000000000000000000000000001 01\n"));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(maintenance::CommandStatus::InvalidSyntax), static_cast<int>(event.status));
  TEST_ASSERT_TRUE(parse("debug level debug\n"));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(maintenance::CommandStatus::InvalidSyntax), static_cast<int>(event.status));
  TEST_ASSERT_TRUE(parse("debug page 00000000000000000000000000000001 +1\n"));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(maintenance::CommandStatus::InvalidSyntax), static_cast<int>(event.status));
}
void test_overrun_counts_skipped_events_once_per_gap() {
  acquisition::BootId boot{}; boot.present=true; boot.bytes[15]=1U;
  Lock lock;
  sigap::debug::DebugService service(boot,"TEST",sigap::debug::Level::Info,lock);
  Writer writer{};
  writer.lock=&lock;
  sigap::debug::UsbDebugSink sink(service,writer);
  sigap::debug::EventInput event{}; event.module=sigap::debug::Module::Runtime; event.event_code=1U;
  for (unsigned i=0U;i<65U;++i) { event.monotonic_us=i; TEST_ASSERT_TRUE(service.emit(event)); }
  sink.poll(100U,1U);
  TEST_ASSERT_EQUAL_UINT64(1U,service.meta(100U).usb_lost);
  sink.poll(100U,384U);
  TEST_ASSERT_EQUAL_UINT64(1U,service.meta(100U).usb_lost);
}
void test_backend_rejects_short_level_ack_without_mutation() {
  acquisition::BootId boot{}; boot.present=true; boot.bytes[15]=1U;
  Lock lock;
  sigap::debug::DebugService service(boot,"TEST",sigap::debug::Level::Info,lock);
  sigap::debug::CommandBackend backend(service);
  char output[16]{}; size_t length=99U;
  TEST_ASSERT_FALSE(backend.set_level(sigap::debug::Level::Error,output,7U,length));
  TEST_ASSERT_EQUAL_UINT32(0U,length);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(sigap::debug::Level::Info),static_cast<uint8_t>(service.meta(0U).level));
  TEST_ASSERT_FALSE(backend.set_level(sigap::debug::Level::Error,nullptr,sizeof(output),length));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(sigap::debug::Level::Info),static_cast<uint8_t>(service.meta(0U).level));
  TEST_ASSERT_FALSE(backend.set_level(static_cast<sigap::debug::Level>(9U),output,sizeof(output),length));
  TEST_ASSERT_EQUAL_UINT32(0U,length);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(sigap::debug::Level::Info),static_cast<uint8_t>(service.meta(0U).level));
  TEST_ASSERT_TRUE(backend.set_level(sigap::debug::Level::Error,output,sizeof(output),length));
  TEST_ASSERT_EQUAL_STRING_LEN("DBG1|L|0",output,length);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(sigap::debug::Level::Error),static_cast<uint8_t>(service.meta(0U).level));
}
void test_partial_usb_delivery_retries_one_complete_codec_line_and_preserves_reader() {
  acquisition::BootId boot{}; boot.present=true; boot.bytes[15]=1U;
  Lock lock;
  sigap::debug::DebugService service(boot,"TEST",sigap::debug::Level::Info,lock);
  Writer writer{}; writer.max_write=3U;
  sigap::debug::UsbDebugSink sink(service,writer);
  sigap::debug::EventInput event{}; event.module=sigap::debug::Module::Runtime;
  event.event_code=1U; event.level=sigap::debug::Level::Info; event.monotonic_us=99U;
  TEST_ASSERT_TRUE(service.emit(event));
  const sigap::debug::Cursor other{boot,0U};
  const auto expected_page=service.read(other);
  char body[384]{}; size_t body_size=0U;
  TEST_ASSERT_TRUE(sigap::debug::encode_page(expected_page,body,sizeof(body),body_size));
  const std::string expected=std::string("SIGAP_DEBUG ")+std::string(body,body_size)+"\n";
  for(unsigned i=0;i<1000U && writer.output.size()<expected.size();++i) {
    writer.allowance=(i%2U)==0U?0U:3U;
    sink.poll(100U,5U);
    TEST_ASSERT_EQUAL_UINT64(0U,service.meta(100U).usb_lost);
  }
  TEST_ASSERT_EQUAL_STRING_LEN(expected.data(),writer.output.data(),expected.size());
  TEST_ASSERT_EQUAL_UINT32(expected.size(),writer.output.size());
  TEST_ASSERT_TRUE(service.read(other).has_event);
  TEST_ASSERT_EQUAL_UINT32(0U,writer.writes_under_lock);
  const std::string delivered=writer.output;
  sink.poll(101U,384U);
  TEST_ASSERT_EQUAL_STRING_LEN(delivered.data(),writer.output.data(),delivered.size());
  TEST_ASSERT_EQUAL_UINT64(1U,service.meta(101U).latest);
}
void test_usb_sink_reconstructs_with_fresh_boot_cursor_without_cross_boot_line() {
  acquisition::BootId boot1{}; boot1.present=true; boot1.bytes[15]=1U;
  acquisition::BootId boot2{}; boot2.present=true; boot2.bytes[15]=2U;
  Lock lock;
  sigap::debug::DebugService first(boot1,"TEST",sigap::debug::Level::Info,lock);
  Writer writer{};
  {
    sigap::debug::UsbDebugSink old_sink(first,writer);
    sigap::debug::EventInput event{}; event.module=sigap::debug::Module::Runtime;
    event.event_code=1U; event.level=sigap::debug::Level::Info;
    TEST_ASSERT_TRUE(first.emit(event));
    old_sink.poll(0U,4U);
    TEST_ASSERT_EQUAL_UINT32(4U,writer.output.size());
  }
  // A restart discards the old in-memory sink and its unsent partial line.
  writer.output.clear();
  sigap::debug::DebugService second(boot2,"TEST",sigap::debug::Level::Info,lock);
  sigap::debug::UsbDebugSink fresh_sink(second,writer);
  sigap::debug::EventInput event{}; event.module=sigap::debug::Module::Runtime;
  event.event_code=1U; event.level=sigap::debug::Level::Info;
  TEST_ASSERT_TRUE(second.emit(event));
  for(unsigned i=0;i<1000U && writer.output.find('\n')==std::string::npos;++i)
    fresh_sink.poll(0U,384U);
  TEST_ASSERT_EQUAL_INT(0,writer.output.find("SIGAP_DEBUG DBG1|P|00000000000000000000000000000002"));
  TEST_ASSERT_EQUAL_UINT32(std::string::npos,writer.output.find("00000000000000000000000000000001"));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(sigap::debug::Level::Info),static_cast<uint8_t>(second.meta(0U).level));
}
void test_parser_rejects_explicit_length_control_and_nul_tokens() {
  const char nul_line[]={'d','e','b','u','g',' ','m','e','t','a','\0','x','\n'};
  for(const char* input : {"debug page 00000000000000000000000000000001 18446744073709551616\n",
                           "debug page 00000000000000000000000000000001 1\x01\n",
                           "debug page 00000000000000000000000000000001 1 x\n"}) {
    maintenance::MaintenanceParser parser{}; maintenance::ParseEvent event{}; bool emitted=false;
    for(size_t i=0;i<std::strlen(input);++i) {
      if(parser.consume(input[i],event)) emitted=true;
    }
    TEST_ASSERT_TRUE(emitted);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(maintenance::CommandStatus::InvalidSyntax),static_cast<int>(event.status));
  }
  maintenance::MaintenanceParser parser{}; maintenance::ParseEvent event{}; bool emitted=false;
  for(char c : nul_line) {
    if(parser.consume(c,event)) emitted=true;
  }
  TEST_ASSERT_TRUE(emitted);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(maintenance::CommandStatus::InvalidSyntax),static_cast<int>(event.status));
}
void test_parser_accepts_crlf_debug_line() {
  maintenance::MaintenanceParser parser{};
  maintenance::ParseEvent event{};
  bool emitted=false;
  for(char c : std::string("debug page 00000000000000000000000000000001 1\r\n")) {
    if(parser.consume(c,event)) {
      emitted=true;
      TEST_ASSERT_EQUAL_INT(static_cast<int>(maintenance::CommandKind::DebugPage),static_cast<int>(event.command.kind));
      TEST_ASSERT_EQUAL_INT(static_cast<int>(maintenance::CommandStatus::Accepted),static_cast<int>(event.status));
    }
  }
  TEST_ASSERT_TRUE(emitted);
}
void test_maximum_meta_and_page_fit_exact_prefixed_usb_capacity() {
  acquisition::BootId boot{}; boot.present=true;
  for(size_t i=0;i<boot.bytes.size();++i) boot.bytes[i]=0xffU;
  sigap::debug::Meta meta{};
  meta.boot=boot;
  meta.build_id.fill('~'); meta.build_id.back()='\0';
  meta.level=sigap::debug::Level::Debug;
  meta.oldest=meta.latest=meta.overwritten=meta.delivery_lost=meta.usb_lost=UINT64_MAX;
  meta.captured_monotonic_us=UINT64_MAX; meta.sequence_exhausted=true;
  struct MetaBuffer { char bytes[362]; char sentinel; } meta_exact_body{};
  meta_exact_body.sentinel='!'; size_t meta_size=0U;
  TEST_ASSERT_TRUE(sigap::debug::encode_meta(meta,meta_exact_body.bytes,sizeof(meta_exact_body.bytes),meta_size));
  TEST_ASSERT_EQUAL_UINT32(362U,meta_size);
  TEST_ASSERT_EQUAL_INT('!',meta_exact_body.sentinel);
  char meta_exact[375]{}; size_t exact_size=0U;
  std::memcpy(meta_exact,"SIGAP_DEBUG ",12U);
  std::memcpy(meta_exact+12U,meta_exact_body.bytes,meta_size); meta_exact[12U+meta_size]='\n';
  TEST_ASSERT_EQUAL_UINT32(375U,12U+meta_size+1U);
  TEST_ASSERT_EQUAL_STRING_LEN("SIGAP_DEBUG ",meta_exact,12U);
  TEST_ASSERT_EQUAL_INT('\n',meta_exact[374U]);
  struct MetaShortBuffer { char bytes[361]; char sentinel; } meta_short{};
  meta_short.sentinel='!'; size_t short_size=99U;
  TEST_ASSERT_FALSE(sigap::debug::encode_meta(meta,meta_short.bytes,sizeof(meta_short.bytes),short_size));
  TEST_ASSERT_EQUAL_UINT32(0U,short_size);
  TEST_ASSERT_EQUAL_INT('!',meta_short.sentinel);

  sigap::debug::Page page{}; page.boot=boot; page.level=sigap::debug::Level::Debug;
  page.oldest=page.next_after=page.overwritten=UINT64_MAX; page.gap=true;
  page.has_event=true; page.event.sequence=UINT64_MAX; page.event.monotonic_us=UINT64_MAX;
  page.event.level=sigap::debug::Level::Debug; page.event.module=sigap::debug::Module::Runtime;
  page.event.event_code=1U; page.event.request_id=UINT32_MAX; page.event.presence=sigap::debug::kHasRequestId;
  page.event.duration_us=UINT32_MAX; page.event.arg0=INT32_MIN; page.event.arg1=INT32_MAX;
  page.event.related_sequence=UINT64_MAX; page.event.presence|=sigap::debug::kHasRelatedSequence;
  char page_body[384]{}; size_t page_size=0U;
  TEST_ASSERT_TRUE(sigap::debug::encode_page(page,page_body,sizeof(page_body),page_size));
  TEST_ASSERT_EQUAL_UINT32(222U,page_size);
  char line[384]{}; size_t line_size=0U;
  std::memcpy(line,"SIGAP_DEBUG ",12U);
  std::memcpy(line+12U,page_body,page_size); line[12U+page_size]='\n';
  line_size=12U+page_size+1U;
  TEST_ASSERT_EQUAL_UINT32(235U,line_size);
  TEST_ASSERT_EQUAL_STRING_LEN("SIGAP_DEBUG ",line,12U);
  TEST_ASSERT_EQUAL_INT('\n',line[line_size-1U]);
  struct PageBuffer { char bytes[223]; char sentinel; } exact_body{};
  exact_body.sentinel='!'; size_t exact_body_size=0U;
  TEST_ASSERT_TRUE(sigap::debug::encode_page(page,exact_body.bytes,page_size,exact_body_size));
  TEST_ASSERT_EQUAL_UINT32(page_size,exact_body_size);
  size_t failed_size=99U;
  TEST_ASSERT_FALSE(sigap::debug::encode_page(page,exact_body.bytes,page_size-1U,failed_size));
  TEST_ASSERT_EQUAL_UINT32(0U,failed_size);
  TEST_ASSERT_EQUAL_INT('!',exact_body.sentinel);
}
void setUp() {}
void tearDown() {}
}
void setup(){std::printf("memory UsbDebugSink=%zu CommandBackend=%zu pending_line=%zu backend_output=%u maintenance_response=%zu\\n",sizeof(sigap::debug::UsbDebugSink),sizeof(sigap::debug::CommandBackend),sizeof(std::array<char,384U>),384U,sizeof(maintenance::MaintenanceResponse));UNITY_BEGIN();RUN_TEST(test_parser_accepts_debug_grammar_and_rejects_noncanonical_cursor);RUN_TEST(test_partial_zero_and_budgeted_usb_delivery);RUN_TEST(test_zero_write_does_not_spin_or_lose_cursor);RUN_TEST(test_overrun_counts_skipped_events_once_per_gap);RUN_TEST(test_backend_rejects_short_level_ack_without_mutation);RUN_TEST(test_partial_usb_delivery_retries_one_complete_codec_line_and_preserves_reader);RUN_TEST(test_usb_sink_reconstructs_with_fresh_boot_cursor_without_cross_boot_line);RUN_TEST(test_parser_rejects_explicit_length_control_and_nul_tokens);RUN_TEST(test_parser_accepts_crlf_debug_line);RUN_TEST(test_maximum_meta_and_page_fit_exact_prefixed_usb_capacity);UNITY_END();}
int main(){setup();return 0;}
