#include "debug/codec.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
namespace {
bool parse_hex(std::string_view text, uint8_t* out, size_t size) {
  if (text.size() != size * 2U) return false;
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < size; ++i) {
    const int high = digit(text[2U * i]);
    const int low = digit(text[2U * i + 1U]);
    if (high < 0 || low < 0) return false;
    out[i] = uint8_t((high << 4) | low);
  }
  return true;
}
sigap::debug::BatchSnapshot fixture_batch() {
  using namespace sigap::debug;
  BatchSnapshot batch{};
  batch.meta.boot.present = true;
  batch.meta.boot.bytes[15] = 1;
  batch.meta.level = Level::Info;
  std::memcpy(batch.meta.build_id.data(), "TEST", 4);
  batch.meta.oldest = 1;
  batch.meta.latest = 2;
  batch.meta.captured_monotonic_us = UINT64_C(9007199254740993);
  batch.count = 1;
  auto& event = batch.events[0];
  event.sequence = 2;
  event.monotonic_us = batch.meta.captured_monotonic_us;
  event.level = Level::Info;
  event.module = Module::Runtime;
  event.event_code = 1;
  event.arg0 = 1;
  return batch;
}
int context_self_test() {
  using namespace sigap::debug;
  BatchSnapshot sent{};
  sent.meta.boot.present = true;
  sent.meta.boot.bytes[15] = 9;
  sent.count = 2;
  sent.events[0].sequence = 2;
  sent.events[1].sequence = 5;
  std::array<uint8_t, 32> digest{};
  UploadAck ack{};
  ack.schema_version = 1;
  std::memcpy(ack.status.data(), "DURABLE", 7);
  ack.device_id_size = 3;
  ack.device_id[0] = 'A'; ack.device_id[1] = '\0'; ack.device_id[2] = 'B';
  ack.boot = sent.meta.boot;
  ack.first_sequence = 2; ack.last_sequence = 5;
  ack.stored_count = 1; ack.deduplicated_count = 1;
  ack.request_digest = digest;
  const std::string_view nul_id(ack.device_id.data(), ack.device_id_size);
  if (!matches_upload_ack(ack, nul_id, sent, digest) ||
      matches_upload_ack(ack, std::string_view("A\0C", 3), sent, digest)) return 50;
  const char utf8_id[] = {'c', 'a', 'f', char(0xC3), char(0xA9)};
  ack.device_id.fill(0);
  std::memcpy(ack.device_id.data(), utf8_id, sizeof(utf8_id));
  ack.device_id_size = sizeof(utf8_id);
  if (!matches_upload_ack(ack, std::string_view(utf8_id, sizeof(utf8_id)), sent, digest)) return 51;
  std::array<uint8_t, 32> wrong_digest = digest;
  wrong_digest[0] = 1;
  if (matches_upload_ack(ack, {}, sent, digest) ||
      matches_upload_ack(ack, std::string_view(utf8_id, sizeof(utf8_id)), sent,
                         wrong_digest)) return 52;
  UploadAck bad = ack;
  if (matches_upload_ack(ack, std::string(65, 'x'), sent, digest)) return 63;
  bad = ack; bad.schema_version = 2;
  if (matches_upload_ack(bad, std::string_view(utf8_id, sizeof(utf8_id)), sent, digest)) return 64;
  bad = ack; bad.status[0] = 'X';
  if (matches_upload_ack(bad, std::string_view(utf8_id, sizeof(utf8_id)), sent, digest)) return 65;
  bad = ack; bad.device_id_size = 65;
  if (matches_upload_ack(bad, std::string_view(utf8_id, sizeof(utf8_id)), sent, digest)) return 53;
  bad = ack; bad.device_id_size = 0;
  if (matches_upload_ack(bad, std::string_view(utf8_id, sizeof(utf8_id)), sent, digest)) return 66;
  bad = ack; bad.boot.present = false;
  if (matches_upload_ack(bad, std::string_view(utf8_id, sizeof(utf8_id)), sent, digest)) return 54;
  bad = ack; bad.stored_count = 2; bad.deduplicated_count = 1;
  if (matches_upload_ack(bad, std::string_view(utf8_id, sizeof(utf8_id)), sent, digest)) return 55;
  bad = ack; bad.first_sequence = 0;
  if (matches_upload_ack(bad, std::string_view(utf8_id, sizeof(utf8_id)), sent, digest)) return 56;
  bad = ack; bad.last_sequence = 4;
  if (matches_upload_ack(bad, std::string_view(utf8_id, sizeof(utf8_id)), sent, digest)) return 57;
  BatchSnapshot invalid = sent; invalid.count = 0;
  if (matches_upload_ack(ack, std::string_view(utf8_id, sizeof(utf8_id)), invalid, digest)) return 58;
  invalid = sent; invalid.count = 9;
  if (matches_upload_ack(ack, std::string_view(utf8_id, sizeof(utf8_id)), invalid, digest)) return 59;
  invalid = sent; invalid.meta.boot.present = false;
  if (matches_upload_ack(ack, std::string_view(utf8_id, sizeof(utf8_id)), invalid, digest)) return 60;
  invalid = sent; invalid.events[1].sequence = 0;
  if (matches_upload_ack(ack, std::string_view(utf8_id, sizeof(utf8_id)), invalid, digest)) return 61;
  invalid = sent; invalid.events[1].sequence = 1;
  if (matches_upload_ack(ack, std::string_view(utf8_id, sizeof(utf8_id)), invalid, digest)) return 62;
  invalid = sent; invalid.events[0].sequence = 0;
  if (matches_upload_ack(ack, std::string_view(utf8_id, sizeof(utf8_id)), invalid, digest)) return 67;
  invalid = sent; invalid.events[0].sequence = 6;
  if (matches_upload_ack(ack, std::string_view(utf8_id, sizeof(utf8_id)), invalid, digest)) return 68;
  const char invalid_utf8[] = {'c', 'a', 'f', char(0xC0), char(0xAF)};
  if (matches_upload_ack(ack, std::string_view(invalid_utf8, sizeof(invalid_utf8)), sent, digest)) return 69;
  std::puts("TEST_CONTEXT=PASS;synthetic=PASS;noncontiguous=2,5;nul+utf8=PASS");
  return 0;
}
}
int main(int argc, char** argv) {
  using namespace sigap::debug;
  if (argc == 2 && std::strcmp(argv[1], "--ble-observation-fixture") == 0) {
    using namespace sigap::debug;
    const BatchSnapshot base = fixture_batch();
    BatchSnapshot batch{};
    batch.meta = base.meta;
    batch.meta.oldest = 20;
    batch.meta.latest = 23;
    batch.count = 4;
    const auto make_event = [&](uint64_t sequence, uint16_t code, int32_t arg0,
                                int32_t arg1, bool request_id, bool related) {
      EventRecord event{};
      event.sequence = sequence;
      event.monotonic_us = batch.meta.captured_monotonic_us;
      event.level = Level::Info;
      event.module = Module::Ble;
      event.event_code = code;
      event.request_id = 0;
      event.arg0 = arg0;
      event.arg1 = arg1;
      event.presence = (request_id ? kHasRequestId : 0U) |
                       (related ? kHasRelatedSequence : 0U);
      if (related) event.related_sequence = 0;
      return event;
    };
    batch.events[0] = make_event(20, 10, 0, 3, true, false);
    batch.events[1] = make_event(21, 10, 1, 3, false, false);
    batch.events[2] = make_event(22, 11, 3, 0, true, true);
    batch.events[3] = make_event(23, 11, 3, 1, false, false);
    char out[4096]{};
    size_t length = 0;
    const char* labels[] = {"PAGE_10_REQUEST_ZERO", "PAGE_10_REQUEST_ABSENT",
                            "PAGE_11_OPTIONALS_ZERO", "PAGE_11_OPTIONALS_ABSENT"};
    for (size_t i = 0; i < batch.count; ++i) {
      Page page{};
      page.boot = batch.meta.boot;
      page.level = batch.meta.level;
      page.oldest = batch.meta.oldest;
      page.next_after = batch.events[i].sequence + 1;
      page.has_event = true;
      page.event = batch.events[i];
      if (!encode_page(page, out, sizeof(out), length)) return 70 + int(i);
      std::printf("%s=%.*s\n", labels[i], int(length), out);
    }
    if (!encode_upload("TEST-DEVICE", batch, out, sizeof(out), length)) return 75;
    std::printf("HTTP_BLE_OBSERVATION=%.*s\n", int(length), out);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--context-self-test") == 0)
    return context_self_test();
  if (argc == 5 && std::strcmp(argv[1], "--match") == 0) {
    std::ifstream input(argv[2], std::ios::binary);
    const std::string raw((std::istreambuf_iterator<char>(input)), {});
    UploadAck ack{};
    std::array<uint8_t, 32> digest{};
    std::array<uint8_t, 64> identity{};
    if (!input.is_open() || !decode_upload_ack(raw, ack) ||
        !parse_hex(argv[3], digest.data(), digest.size()) ||
        (std::strlen(argv[4]) % 2U) != 0 || std::strlen(argv[4]) > identity.size() * 2U)
      return 41;
    const size_t identity_size = std::strlen(argv[4]) / 2U;
    if (!parse_hex(argv[4], identity.data(), identity_size)) return 42;
    const BatchSnapshot sent = fixture_batch();
    const bool matched = matches_upload_ack(ack,
        std::string_view(reinterpret_cast<const char*>(identity.data()), identity_size),
        sent, digest);
    std::printf("MATCH=%u\n", matched ? 1U : 0U);
    return 0;
  }
  if (argc == 3 && std::strcmp(argv[1], "--ack") == 0) {
    std::ifstream input(argv[2], std::ios::binary);
    const std::string raw((std::istreambuf_iterator<char>(input)), {});
    UploadAck ack{};
    if (!input.is_open() || !decode_upload_ack(raw, ack)) return 40;
    std::printf("ACK=%u|%s|%u|", unsigned(ack.schema_version), ack.status.data(),
                unsigned(ack.device_id_size));
    for (size_t i = 0; i < ack.device_id_size; ++i)
      std::printf("%02x", unsigned(static_cast<uint8_t>(ack.device_id[i])));
    std::putchar('|');
    for (uint8_t byte : ack.boot.bytes) std::printf("%02x", unsigned(byte));
    std::printf("|%llu|%llu|%u|%u|",
                static_cast<unsigned long long>(ack.first_sequence),
                static_cast<unsigned long long>(ack.last_sequence),
                unsigned(ack.stored_count), unsigned(ack.deduplicated_count));
    for (uint8_t byte : ack.request_digest) std::printf("%02x", unsigned(byte));
    std::putchar('\n');
    return 0;
  }
  const BatchSnapshot request = fixture_batch();
  const Meta meta = request.meta;
  const EventRecord baseline_event = request.events[0];
  char out[4096]{}; size_t length=0;
  if(!encode_meta(meta,out,512,length)) return 1;
  std::printf("META=%.*s\n",int(length),out);
  Page page{}; page.boot=meta.boot; page.level=meta.level; page.oldest=1; page.next_after=2; page.has_event=true;
  page.event=baseline_event;
  if(!encode_page(page,out,512,length)) return 2;
  std::printf("PAGE=%.*s\n",int(length),out);
  Page cursor_page{}; cursor_page.boot=meta.boot; cursor_page.level=meta.level;
  cursor_page.oldest=1; cursor_page.has_event=true; cursor_page.event=baseline_event;
  cursor_page.next_after=1; length=77;
  if(encode_page(cursor_page,out,512,length)||length!=0) return 37;
  cursor_page.next_after=2;
  if(!encode_page(cursor_page,out,512,length)) return 38;
  cursor_page.next_after=3;
  if(!encode_page(cursor_page,out,512,length)) return 39;
  if(!encode_event_json(baseline_event,out,512,length)) return 3;
  std::printf("EVENT=%.*s\n",int(length),out);
  Page empty{}; empty.boot=meta.boot; empty.level=Level::Info;
  if(!encode_page(empty,out,512,length)) return 4;
  std::printf("EMPTY_PAGE=%.*s\n",int(length),out);
  EventRecord max_event{}; max_event.sequence=UINT64_MAX; max_event.monotonic_us=UINT64_MAX; max_event.level=Level::Debug;
  max_event.module=Module::Runtime; max_event.event_code=1; max_event.request_id=UINT32_MAX;
  max_event.duration_us=UINT32_MAX; max_event.arg0=INT32_MIN; max_event.arg1=INT32_MAX;
  max_event.related_sequence=UINT64_MAX; max_event.presence=kHasRequestId|kHasRelatedSequence;
  page.event=max_event; page.oldest=UINT64_MAX; page.overwritten=UINT64_MAX; page.next_after=UINT64_MAX;
  if(!encode_page(page,out,512,length)) return 5;
  std::printf("MAX_PAGE=%.*s\n",int(length),out);
  BatchSnapshot batch=request;
  if(!encode_upload("TEST-DEVICE",batch,out,sizeof(out),length)) return 6;
  std::printf("HTTP=%.*s\n",int(length),out);
  const std::string_view upload(out,length);
  batch.events[0]=max_event;
  length=99;
  if(encode_upload("TEST-DEVICE",batch,out,sizeof(out),length)||length!=0) return 9;
  batch.events[0]=baseline_event;
  batch.meta.overwritten=UINT64_MAX; batch.meta.delivery_lost=0;
  if(!encode_upload("TEST-DEVICE",batch,out,sizeof(out),length)) return 10;
  std::printf("COUNTERS_MAX=%.*s\n",int(length),out);
  batch.meta.overwritten=0; batch.meta.delivery_lost=UINT64_MAX;
  if(!encode_upload("TEST-DEVICE",batch,out,sizeof(out),length)) return 11;
  std::printf("COUNTERS_ZERO=%.*s\n",int(length),out);
  batch.meta=meta; batch.events[0]=baseline_event;
  const std::string escaped_id="ID\"\\\n";
  if(!encode_upload(escaped_id,batch,out,sizeof(out),length)) return 12;
  std::printf("HTTP_ESCAPED=%.*s\n",int(length),out);
  const auto exact=length;
  char exact_out[4096]{}; size_t exact_len=0;
  if(!encode_upload(escaped_id,batch,exact_out,exact,exact_len)||exact_len!=exact) return 13;
  std::memset(exact_out,'#',sizeof(exact_out)); exact_len=77;
  if(encode_upload(escaped_id,batch,exact_out,exact-1,exact_len)||exact_len!=0||exact_out[exact-1]!='#') return 14;
  const char bad_utf8[]={char(0xC0),char(0xAF)};
  if(encode_upload(std::string_view(bad_utf8,2),batch,out,sizeof(out),length)) return 15;
  Meta maximum{}; maximum.boot=meta.boot; maximum.level=Level::Debug;
  std::memset(maximum.build_id.data(),'X',96); maximum.oldest=UINT64_MAX; maximum.latest=UINT64_MAX;
  maximum.overwritten=maximum.delivery_lost=maximum.usb_lost=maximum.captured_monotonic_us=UINT64_MAX;
  maximum.sequence_exhausted=true;
  if(!encode_meta(maximum,out,512,length)||length>512) return 7;
  char max_meta[512]{}; size_t max_len=0;
  if(!encode_meta(maximum,max_meta,sizeof(max_meta),max_len)||max_len>512) return 8;
  std::printf("MAX_META=%.*s\n",int(max_len),max_meta);
  std::memset(max_meta,'#',sizeof(max_meta)); size_t short_len=55;
  if(encode_meta(maximum,max_meta,max_len-1,short_len)||short_len!=0||max_meta[max_len-1]!='#') return 16;
  EventRecord absent=baseline_event;
  if(!encode_event_json(absent,out,sizeof(out),length)) return 17;
  std::printf("EVENT_ABSENT=%.*s\n",int(length),out);
  char page_exact[512]{}; size_t page_size=0;
  if(!encode_page(page,page_exact,sizeof(page_exact),page_size)||!encode_page(page,out,page_size,length)||length!=page_size) return 25;
  std::memset(out,'#',sizeof(out)); length=88;
  if(encode_page(page,out,page_size-1,length)||length!=0||out[page_size-1]!='#') return 26;
  absent.presence=kHasRequestId|kHasRelatedSequence; absent.request_id=0; absent.related_sequence=0;
  if(!encode_event_json(absent,out,sizeof(out),length)) return 18;
  std::printf("EVENT_ZERO=%.*s\n",int(length),out);
  EventRecord bad=baseline_event; bad.presence=0x80;
  if(encode_event_json(bad,out,sizeof(out),length)) return 27;
  bad=baseline_event; bad.module=static_cast<Module>(99);
  if(encode_event_json(bad,out,sizeof(out),length)) return 28;
  bad=baseline_event; bad.level=static_cast<Level>(99);
  if(encode_event_json(bad,out,sizeof(out),length)) return 29;
  bad=max_event;
  if(!encode_event_json(bad,out,512,length)||length>512) return 30;
  std::printf("MAX_EVENT=%.*s\n",int(length),out);
  BatchSnapshot full{}; full.meta=meta; full.meta.latest=8; full.meta.captured_monotonic_us=UINT64_MAX; full.count=8;
  for(size_t i=0;i<full.count;++i){full.events[i]=baseline_event;full.events[i].sequence=i+1;}
  if(!encode_upload("TEST-DEVICE",full,out,sizeof(out),length)||length>4096) return 31;
  std::printf("HTTP_EIGHT=%.*s\n",int(length),out);
  BatchSnapshot bad_batch=batch; bad_batch.meta=meta; bad_batch.count=0;
  if(encode_upload("TEST-DEVICE",bad_batch,out,sizeof(out),length)) return 32;
  bad_batch=batch; bad_batch.count=9;
  if(encode_upload("TEST-DEVICE",bad_batch,out,sizeof(out),length)) return 35;
  bad_batch=batch; bad_batch.count=2; bad_batch.events[1]=baseline_event;
  if(encode_upload("TEST-DEVICE",bad_batch,out,sizeof(out),length)) return 36;
  bad_batch=batch; bad_batch.events[0].monotonic_us=meta.captured_monotonic_us+1;
  if(encode_upload("TEST-DEVICE",bad_batch,out,sizeof(out),length)) return 33;
  bad_batch=batch; bad_batch.events[0]=max_event;
  if(encode_upload("TEST-DEVICE",bad_batch,out,sizeof(out),length)) return 34;
  Page flags=empty; flags.gap=true; flags.boot_changed=true;
  if(!encode_page(flags,out,512,length)) return 19;
  std::printf("GAP_PAGE=%.*s\n",int(length),out);
  Page zero_optional=page; zero_optional.event=baseline_event;
  zero_optional.event.presence=kHasRequestId|kHasRelatedSequence;
  zero_optional.event.request_id=0; zero_optional.event.related_sequence=0;
  if(!encode_page(zero_optional,out,512,length)) return 23;
  std::printf("PAGE_ZERO=%.*s\n",int(length),out);
  Page invalid=page; invalid.event.sequence=0;
  if(encode_page(invalid,out,512,length)) return 20;
  Meta no_build=meta; no_build.build_id.fill(0);
  if(encode_meta(no_build,out,512,length)) return 21;
  Page missing{}; missing.level=Level::Info;
  if(encode_page(missing,out,512,length)) return 22;
  std::printf("UPLOAD_BASE_BYTES=%zu\n",upload.size());
  return 0;
}
