#include "debug/codec.h"
#include "debug/registry.h"
#include <cstdio>
#include <cstring>
namespace sigap::debug {
namespace { const char* level_name(Level v) noexcept { switch(v){case Level::Error:return "ERROR";case Level::Warn:return "WARN";case Level::Info:return "INFO";case Level::Debug:return "DEBUG";}return nullptr;} bool boot_text(const acquisition::BootId& b,char* out) noexcept {if(!b.present)return false;static constexpr char h[]="0123456789abcdef";for(size_t i=0;i<16;i++){out[2*i]=h[b.bytes[i]>>4U];out[2*i+1]=h[b.bytes[i]&15U];}out[32]=0;return true;} }
namespace {
class Writer {
 public:
  Writer(char* out, size_t cap) noexcept : out_(out), cap_(cap) {}
  bool text(const char* s, size_t n) noexcept {
    if (!out_ || n > cap_ - used_) return false;
    std::memcpy(out_ + used_, s, n); used_ += n; return true;
  }
  template <size_t N> bool text(const char (&s)[N]) noexcept { return text(s, N - 1); }
  bool ch(char c) noexcept { return text(&c, 1); }
  bool number(uint64_t v) noexcept {
    char b[20]; size_t n = 0;
    do { b[n++] = char('0' + v % 10U); v /= 10U; } while (v);
    while (n) if (!ch(b[--n])) return false;
    return true;
  }
  bool signed_number(int32_t v) noexcept {
    if (v < 0 && !ch('-')) return false;
    uint32_t n = v < 0 ? uint32_t(-(int64_t(v))) : uint32_t(v);
    char b[10]; size_t len = 0;
    do { b[len++] = char('0' + n % 10U); n /= 10U; } while (n);
    while (len) if (!ch(b[--len])) return false;
    return true;
  }
  bool decimal(uint64_t v) noexcept { return ch('"') && number(v) && ch('"'); }
  size_t size() const noexcept { return used_; }
 private:
  char* out_; size_t cap_; size_t used_{};
};
bool valid_event(const EventRecord& e) noexcept {
  return e.sequence != 0 && level_name(e.level) && registered(e.module, e.event_code) &&
      !(e.presence & uint8_t(~(kHasRequestId | kHasRelatedSequence)));
}
bool append_event_json(Writer& w, const EventRecord& e) noexcept {
  const char* level = level_name(e.level);
  return valid_event(e) && w.text("{\"sequence\":") && w.decimal(e.sequence) &&
      w.text(",\"monotonic_us\":") && w.decimal(e.monotonic_us) &&
      w.text(",\"level\":\"") && w.text(level, std::strlen(level)) &&
      w.text("\",\"module\":") && w.number(uint8_t(e.module)) &&
      w.text(",\"event_code\":") && w.number(e.event_code) &&
      w.text(",\"request_id\":") &&
      ((e.presence & kHasRequestId) ? w.number(e.request_id) : w.text("null")) &&
      w.text(",\"duration_us\":") && w.number(e.duration_us) &&
      w.text(",\"arg0\":") && w.signed_number(e.arg0) &&
      w.text(",\"arg1\":") && w.signed_number(e.arg1) &&
      w.text(",\"related_sequence\":") &&
      ((e.presence & kHasRelatedSequence) ? w.decimal(e.related_sequence) : w.text("null")) && w.ch('}');
}
bool valid_utf8(std::string_view s) noexcept {
  size_t i = 0;
  while (i < s.size()) {
    const uint8_t c = uint8_t(s[i++]);
    if (c < 0x80) continue;
    unsigned extra = c >= 0xC2 && c <= 0xDF ? 1U : c >= 0xE0 && c <= 0xEF ? 2U : c >= 0xF0 && c <= 0xF4 ? 3U : 9U;
    if (extra == 9U || i + extra > s.size()) return false;
    uint32_t cp = c & (extra == 1 ? 0x1FU : extra == 2 ? 0x0FU : 0x07U);
    for (unsigned j = 0; j < extra; ++j) { uint8_t d = uint8_t(s[i++]); if ((d & 0xC0U) != 0x80U) return false; cp = (cp << 6U) | (d & 0x3FU); }
    if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
  }
  return true;
}
bool json_string(Writer& w, std::string_view s) noexcept {
  if (!w.ch('"')) return false;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { if (!w.ch('\\') || !w.ch(char(c))) return false; }
    else if (c < 0x20) { static constexpr char hex[]="0123456789abcdef"; if (!w.text("\\u00",4)||!w.ch(hex[c>>4U])||!w.ch(hex[c&15U])) return false; }
    else if (!w.ch(char(c))) return false;
  }
  return w.ch('"');
}
bool valid_meta(const Meta& m) noexcept {
  if (!m.boot.present || !level_name(m.level)) return false;
  if ((m.oldest == 0) != (m.latest == 0) || m.oldest > m.latest) return false;
  size_t n = 0;
  while (n < m.build_id.size() && m.build_id[n]) { unsigned char c=m.build_id[n]; if (c<32 || c>126 || n==96) return false; ++n; }
  return n >= 1 && n <= 96 && (n == m.build_id.size() || m.build_id[n] == 0);
}
}
bool encode_meta(const Meta& m, char* out, size_t cap, size_t& len) noexcept {
  len=0;
  if (!out || !cap || !valid_meta(m)) return false;
  char boot[33]; if (!boot_text(m.boot, boot)) return false;
  size_t build=0; while (build<m.build_id.size() && m.build_id[build]) ++build;
  Writer w(out, cap); static constexpr char hex[]="0123456789abcdef";
  if (!w.text("DBG1|M|") || !w.text(boot,32) || !w.ch('|')) return false;
  for(size_t i=0;i<build;++i){uint8_t c=uint8_t(m.build_id[i]);if(!w.ch(hex[c>>4U])||!w.ch(hex[c&15U]))return false;}
  if(!w.ch('|')||!w.number(uint8_t(m.level))||!w.ch('|')||!w.number(m.oldest)||!w.ch('|')||!w.number(m.latest)||!w.ch('|')||!w.number(m.overwritten)||!w.ch('|')||!w.number(m.delivery_lost)||!w.ch('|')||!w.number(m.usb_lost)||!w.ch('|')||!w.number(m.captured_monotonic_us)||!w.ch('|')||!w.ch(m.sequence_exhausted?'1':'0'))return false;
  len=w.size();return true;
}
bool encode_page(const Page& p, char* out, size_t cap, size_t& len) noexcept {
  len=0;if(!out||!cap||!p.boot.present||!level_name(p.level))return false;
  char boot[33];if(!boot_text(p.boot,boot))return false;
  Writer w(out,cap);
  if(!w.text("DBG1|P|")||!w.text(boot,32)||!w.ch('|')||!w.number(uint8_t(p.level))||!w.ch('|')||!w.number(p.oldest)||!w.ch('|')||!w.number(p.next_after)||!w.ch('|')||!w.number(p.overwritten)||!w.ch('|')||!w.ch(p.gap?'1':'0')||!w.ch('|')||!w.ch(p.boot_changed?'1':'0')||!w.ch('|'))return false;
  if(!p.has_event){if(!w.ch('-'))return false;len=w.size();return len<=512;}
  const auto& e=p.event;
  if(!valid_event(e)||e.sequence>p.next_after||(!(e.presence&kHasRequestId) && e.request_id!=0))return false;
  if(!w.number(e.sequence)||!w.ch(',')||!w.number(e.monotonic_us)||!w.ch(',')||!w.number(uint8_t(e.level))||!w.ch(',')||!w.number(uint8_t(e.module))||!w.ch(',')||!w.number(e.event_code)||!w.ch(','))return false;
  if(e.presence&kHasRequestId){if(!w.number(e.request_id))return false;}else if(!w.ch('-'))return false;
  if(!w.ch(',')||!w.number(e.duration_us)||!w.ch(',')||!w.signed_number(e.arg0)||!w.ch(',')||!w.signed_number(e.arg1)||!w.ch(','))return false;
  if(e.presence&kHasRelatedSequence){if(!w.number(e.related_sequence))return false;}else if(!w.ch('-'))return false;
  len=w.size();return len<=512;
}
bool encode_event_json(const EventRecord& e,char* out,size_t cap,size_t& len) noexcept {
  len=0;if(!out||!cap)return false;
  Writer w(out,cap);
  if(!append_event_json(w,e))return false;
  len=w.size();return len<=512;
}
bool encode_upload(std::string_view device_id,const BatchSnapshot& batch,char* out,size_t cap,size_t& len) noexcept {
  len=0;if(!out||!cap||device_id.empty()||device_id.size()>64||!valid_utf8(device_id)||!valid_meta(batch.meta)||batch.count<1||batch.count>batch.events.size())return false;
  size_t build=0;while(build<batch.meta.build_id.size()&&batch.meta.build_id[build])++build;
  Writer w(out,cap);char boot[33];if(!boot_text(batch.meta.boot,boot))return false;
  if(!w.text("{\"schema_version\":1,\"device_id\":")||!json_string(w,device_id)||!w.text(",\"boot_id\":\"")||!w.text(boot,32)||!w.text("\",\"build_id\":")||!json_string(w,{batch.meta.build_id.data(),build})||!w.text(",\"level\":\"")||!w.text(level_name(batch.meta.level),std::strlen(level_name(batch.meta.level)))||!w.text("\",\"captured_monotonic_us\":")||!w.decimal(batch.meta.captured_monotonic_us)||!w.text(",\"oldest_sequence\":")||!w.decimal(batch.meta.oldest)||!w.text(",\"overwritten\":")||!w.decimal(batch.meta.overwritten)||!w.text(",\"delivery_lost\":")||!w.decimal(batch.meta.delivery_lost)||!w.text(",\"events\":["))return false;
  uint64_t previous=0;
  for(size_t i=0;i<batch.count;++i){
    const auto& e=batch.events[i];
    if(!valid_event(e)||e.sequence<=previous||e.monotonic_us>batch.meta.captured_monotonic_us||e.sequence>batch.meta.latest)return false;
    if(i&&!w.ch(','))return false;
    char event_bytes[512];Writer event(event_bytes,sizeof(event_bytes));
    if(!append_event_json(event,e)||event.size()>512||!w.text(event_bytes,event.size()))return false;
    previous=e.sequence;
  }
  if(!w.text("]}")||w.size()>4096)return false;
  len=w.size();return true;
}
bool decode_upload_ack(std::string_view raw, UploadAck& ack) noexcept {
  if (raw.size() > 4096) return false;

  struct Reader {
    std::string_view text;
    size_t position{};
    char string_buffer[65]{};

    void whitespace() noexcept {
      while (position < text.size() &&
             (text[position] == ' ' || text[position] == '\n' ||
              text[position] == '\r' || text[position] == '\t')) {
        ++position;
      }
    }
    bool expect(char value) noexcept {
      whitespace();
      if (position == text.size() || text[position] != value) return false;
      ++position;
      return true;
    }
    bool string(std::string_view& value) noexcept {
      whitespace();
      if (position == text.size() || text[position++] != '"') return false;
      size_t size = 0;
      auto append = [&](uint8_t byte) noexcept {
        if (size == sizeof(string_buffer) - 1U) return false;
        string_buffer[size++] = char(byte);
        return true;
      };
      auto hex4 = [&](uint16_t& codepoint) noexcept {
        if (text.size() - position < 4U) return false;
        codepoint = 0;
        for (unsigned i = 0; i < 4; ++i) {
          const char digit = text[position++];
          const int nibble = digit >= '0' && digit <= '9' ? digit - '0' :
              digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 :
              digit >= 'A' && digit <= 'F' ? digit - 'A' + 10 : -1;
          if (nibble < 0) return false;
          codepoint = uint16_t((codepoint << 4U) | unsigned(nibble));
        }
        return true;
      };
      auto append_codepoint = [&](uint32_t cp) noexcept {
        if (cp < 0x80U) return append(uint8_t(cp));
        if (cp < 0x800U)
          return append(uint8_t(0xC0U | (cp >> 6U))) && append(uint8_t(0x80U | (cp & 0x3FU)));
        if (cp < 0x10000U)
          return append(uint8_t(0xE0U | (cp >> 12U))) && append(uint8_t(0x80U | ((cp >> 6U) & 0x3FU))) && append(uint8_t(0x80U | (cp & 0x3FU)));
        return append(uint8_t(0xF0U | (cp >> 18U))) && append(uint8_t(0x80U | ((cp >> 12U) & 0x3FU))) &&
            append(uint8_t(0x80U | ((cp >> 6U) & 0x3FU))) && append(uint8_t(0x80U | (cp & 0x3FU)));
      };
      while (position < text.size() && text[position] != '"') {
        const uint8_t ch = uint8_t(text[position++]);
        if (ch < 0x20U) return false;
        if (ch != '\\') {
          if (!append(ch)) return false;
          continue;
        }
        if (position == text.size()) return false;
        const char escaped = text[position++];
        switch (escaped) {
          case '"': case '\\': case '/': if (!append(uint8_t(escaped))) return false; break;
          case 'b': if (!append(0x08U)) return false; break;
          case 'f': if (!append(0x0CU)) return false; break;
          case 'n': if (!append(0x0AU)) return false; break;
          case 'r': if (!append(0x0DU)) return false; break;
          case 't': if (!append(0x09U)) return false; break;
          case 'u': {
            uint16_t first = 0;
            if (!hex4(first)) return false;
            uint32_t cp = first;
            if (first >= 0xD800U && first <= 0xDBFFU) {
              if (text.size() - position < 6U || text[position] != '\\' || text[position + 1U] != 'u') return false;
              position += 2U;
              uint16_t second = 0;
              if (!hex4(second) || second < 0xDC00U || second > 0xDFFFU) return false;
              cp = 0x10000U + ((uint32_t(first) - 0xD800U) << 10U) + (uint32_t(second) - 0xDC00U);
            } else if (first >= 0xDC00U && first <= 0xDFFFU) return false;
            if (!append_codepoint(cp)) return false;
            break;
          }
          default: return false;
        }
      }
      if (position == text.size()) return false;
      if (!valid_utf8(std::string_view(string_buffer, size))) return false;
      value = std::string_view(string_buffer, size);
      ++position;
      return true;
    }
    bool unsigned_integer(uint64_t& value) noexcept {
      whitespace();
      const size_t begin = position;
      while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
        ++position;
      }
      if (begin == position || (position - begin > 1 && text[begin] == '0')) return false;
      value = 0;
      for (size_t i = begin; i < position; ++i) {
        const uint64_t digit = uint64_t(text[i] - '0');
        if (value > (UINT64_MAX - digit) / 10U) return false;
        value = value * 10U + digit;
      }
      return true;
    }
    bool positive_decimal_string(uint64_t& value) noexcept {
      std::string_view digits;
      if (!string(digits) || digits.empty() ||
          (digits.size() > 1 && digits.front() == '0')) return false;
      value = 0;
      for (char ch : digits) {
        if (ch < '0' || ch > '9') return false;
        const uint64_t digit = uint64_t(ch - '0');
        if (value > (UINT64_MAX - digit) / 10U) return false;
        value = value * 10U + digit;
      }
      return value != 0;
    }
  } reader{raw};

  UploadAck decoded{};
  uint16_t seen = 0;
  if (!reader.expect('{')) return false;
  for (;;) {
    std::string_view key;
    if (!reader.string(key) || !reader.expect(':')) return false;
    uint16_t field = 0;
    if (key == "schema_version") field = 1U;
    else if (key == "status") field = 2U;
    else if (key == "device_id") field = 4U;
    else if (key == "boot_id") field = 8U;
    else if (key == "first_sequence") field = 16U;
    else if (key == "last_sequence") field = 32U;
    else if (key == "stored_count") field = 64U;
    else if (key == "deduplicated_count") field = 128U;
    else if (key == "request_sha256") field = 256U;
    else return false;
    if (seen & field) return false;
    seen = uint16_t(seen | field);

    uint64_t number = 0;
    if (field == 1U || field == 64U || field == 128U) {
      if (!reader.unsigned_integer(number)) return false;
      if (field == 1U) {
        if (number != 1) return false;
        decoded.schema_version = 1;
      } else {
        if (number > 8) return false;
        if (field == 64U) decoded.stored_count = uint8_t(number);
        else decoded.deduplicated_count = uint8_t(number);
      }
    } else if (field == 16U || field == 32U) {
      if (!reader.positive_decimal_string(number)) return false;
      if (field == 16U) decoded.first_sequence = number;
      else decoded.last_sequence = number;
    } else {
      std::string_view value;
      if (!reader.string(value)) return false;
      if (field == 2U) {
        if (value != "DURABLE") return false;
        std::memcpy(decoded.status.data(), value.data(), value.size());
      } else if (field == 4U) {
        if (value.empty() || value.size() > 64) return false;
        std::memcpy(decoded.device_id.data(), value.data(), value.size());
        decoded.device_id_size = uint8_t(value.size());
      } else {
        const size_t expected = field == 8U ? 32U : 64U;
        if (value.size() != expected) return false;
        auto hex_digit = [](char ch) noexcept -> int {
          if (ch >= '0' && ch <= '9') return ch - '0';
          if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
          return -1;
        };
        const size_t bytes = expected / 2U;
        for (size_t i = 0; i < bytes; ++i) {
          const int high = hex_digit(value[2U * i]);
          const int low = hex_digit(value[2U * i + 1U]);
          if (high < 0 || low < 0) return false;
          const uint8_t byte = uint8_t((high << 4) | low);
          if (field == 8U) decoded.boot.bytes[i] = byte;
          else decoded.request_digest[i] = byte;
        }
        if (field == 8U) decoded.boot.present = true;
      }
    }

    reader.whitespace();
    if (reader.position < raw.size() && raw[reader.position] == ',') {
      ++reader.position;
      continue;
    }
    break;
  }

  if (!reader.expect('}') || seen != 511U) return false;
  reader.whitespace();
  if (reader.position != raw.size() || decoded.first_sequence > decoded.last_sequence ||
      unsigned(decoded.stored_count) + unsigned(decoded.deduplicated_count) == 0U ||
      unsigned(decoded.stored_count) + unsigned(decoded.deduplicated_count) > 8U) {
    return false;
  }
  ack = decoded;
  return true;
}
bool matches_upload_ack(const UploadAck& ack, std::string_view expected_device_id,
                        const BatchSnapshot& sent,
                        const std::array<uint8_t,32>& expected_digest) noexcept {
  if (expected_device_id.empty() || expected_device_id.size() > 64 ||
      !valid_utf8(expected_device_id) || ack.device_id_size > 64 ||
      ack.device_id_size != expected_device_id.size() || sent.count < 1 ||
      sent.count > sent.events.size() || ack.schema_version != 1 ||
      std::memcmp(ack.status.data(), "DURABLE", 7) != 0 || ack.status[7] != '\0' ||
      !sent.meta.boot.present || !ack.boot.present ||
      sent.events[0].sequence == 0 || sent.events[sent.count - 1].sequence == 0 ||
      sent.events[0].sequence > sent.events[sent.count - 1].sequence) return false;
  for (size_t i = 1; i < sent.count; ++i) {
    if (sent.events[i].sequence == 0 ||
        sent.events[i - 1].sequence >= sent.events[i].sequence) return false;
  }
  return std::memcmp(ack.device_id.data(), expected_device_id.data(),
                     expected_device_id.size()) == 0 &&
      ack.boot.bytes == sent.meta.boot.bytes &&
      ack.first_sequence == sent.events[0].sequence &&
      ack.last_sequence == sent.events[sent.count - 1].sequence &&
      unsigned(ack.stored_count) + unsigned(ack.deduplicated_count) == sent.count &&
      ack.request_digest == expected_digest;
}
}
