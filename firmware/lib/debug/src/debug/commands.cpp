#include "debug/commands.h"
#include "debug/codec.h"
#include <charconv>
namespace sigap::debug {
bool CommandBackend::meta(uint64_t now_us, char* out, size_t capacity, size_t& length) noexcept {
  return encode_meta(service_.meta(now_us), out, capacity, length);
}
bool CommandBackend::page(const Cursor& cursor, char* out, size_t capacity, size_t& length) noexcept {
  return encode_page(service_.read(cursor), out, capacity, length);
}
bool CommandBackend::set_level(Level level, char* out, size_t capacity, size_t& length) noexcept {
  length = 0U;
  constexpr char prefix[] = "DBG1|L|";
  constexpr size_t needed = sizeof(prefix) - 1U + 1U;
  if (out == nullptr || capacity < needed || static_cast<uint8_t>(level) > 3U) return false;
  if (!service_.set_level(level)) return false;
  for (size_t i = 0; i < sizeof(prefix) - 1U; ++i) out[i] = prefix[i];
  out[needed - 1U] = static_cast<char>('0' + static_cast<uint8_t>(service_.meta(0U).level));
  length = needed;
  return true;
}
maintenance::CommandStatus CommandBackend::execute_debug(
    const maintenance::MaintenanceCommand& command, uint64_t now_us,
    maintenance::MaintenanceResponse& response) noexcept {
  char output[384U]{}; size_t length = 0U; bool ok = false;
  if (command.kind == maintenance::CommandKind::DebugLevel &&
      response.size() > maintenance::kMaxResponseBytes - 21U) {
    return maintenance::CommandStatus::Failed;
  }
  if (command.kind == maintenance::CommandKind::DebugMeta) {
    ok = meta(now_us, output, sizeof(output), length);
  } else if (command.kind == maintenance::CommandKind::DebugPage) {
    Cursor cursor{};
    if (command.target.size != 32U) return maintenance::CommandStatus::InvalidSyntax;
    for (size_t i=0; i<16U; ++i) {
      auto nibble=[](char c)->int { if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; return -1; };
      const int hi=nibble(command.target.bytes[i*2U]), lo=nibble(command.target.bytes[i*2U+1U]);
      if(hi<0||lo<0)return maintenance::CommandStatus::InvalidSyntax;
      cursor.boot.bytes[i]=static_cast<uint8_t>((hi<<4)|lo);
    }
    cursor.boot.present=true;
    uint64_t after=0U; const auto value=command.value.view();
    const auto parsed=std::from_chars(value.data(),value.data()+value.size(),after);
    if(value.empty()||parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size()||
       (value.size()>1U&&value.front()=='0'))return maintenance::CommandStatus::InvalidSyntax;
    cursor.after=after; ok=page(cursor,output,sizeof(output),length);
  } else if (command.kind == maintenance::CommandKind::DebugLevel) {
    Level level{}; const auto value=command.target.view();
    if(value=="ERROR")level=Level::Error; else if(value=="WARN")level=Level::Warn;
    else if(value=="INFO")level=Level::Info; else if(value=="DEBUG")level=Level::Debug;
    else return maintenance::CommandStatus::InvalidSyntax;
    ok=set_level(level,output,sizeof(output),length);
  } else return maintenance::CommandStatus::Rejected;
  if(!ok)return maintenance::CommandStatus::Failed;
  if(!response.append("SIGAP_DEBUG ")||!response.append(std::string_view(output,length))||!response.append("\n"))
    return maintenance::CommandStatus::Failed;
  return maintenance::CommandStatus::Accepted;
}
}
