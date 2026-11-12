#include "debug/http_transport.h"
#include "network/network.h"
#ifdef ARDUINO
#include <algorithm>
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
namespace sigap::debug {
namespace {
class AckCollector final : public Stream {
 public:
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t byte) override {
    if (size_==bytes_.size()) { truncated_=true; return 0U; }
    bytes_[size_++]=static_cast<char>(byte); return 1U;
  }
  const std::array<char,512>& bytes() const noexcept { return bytes_; }
  size_t size() const noexcept { return size_; }
  bool truncated() const noexcept { return truncated_; }
 private:
  std::array<char,512> bytes_{};
  size_t size_{};
  bool truncated_{};
};
bool copy_endpoint(std::string_view in, std::array<char,193>& out) noexcept {
  constexpr std::string_view prefix{"https://"};
  if (in.size() <= prefix.size() || in.size() >= out.size() ||
      in.substr(0,prefix.size()) != prefix || in.find_first_of("/?#",prefix.size()) == prefix.size()) return false;
  for (unsigned char c: in) if (c <= 0x20 || c == 0x7f) return false;
  std::copy(in.begin(),in.end(),out.begin()); out[in.size()]='\0'; return true;
}
}
DebugHttpsTransport::DebugHttpsTransport(std::string_view endpoint,const char* ca) noexcept : ca_(ca) {
  if (!copy_endpoint(endpoint,endpoint_)) endpoint_[0]='\0';
}
bool DebugHttpsTransport::quiescent() const noexcept {
  if (!mutex_) return state_==State::Idle && is_quiescent_;
  if (xSemaphoreTake(mutex_,0U)!=pdTRUE) return false;
  const bool value=is_quiescent_; xSemaphoreGive(mutex_); return value;
}
network::TransportResult DebugHttpsTransport::start(std::string_view body,std::string_view token,uint32_t timeout) noexcept {
  if (!endpoint_[0] || !ca_ || !ca_[0] || body.empty() || token.empty() || token.size()>128U || timeout==0U || timeout>UINT16_MAX || !WiFi.isConnected()) return network::TransportResult::Failed;
  if (!mutex_) mutex_=xSemaphoreCreateMutex();
  if (!mutex_ || xSemaphoreTake(mutex_,0U)!=pdTRUE) return network::TransportResult::Failed;
  if (state_!=State::Idle || !is_quiescent_) { xSemaphoreGive(mutex_); return network::TransportResult::Failed; }
  body_=body.data(); body_size_=body.size(); token_=token.data(); token_size_=token.size(); timeout_ms_=timeout;
  cancel_requested_=false; result_ready_=false; result_published_=false; is_quiescent_=false; response_={}; state_=State::Running;
  xSemaphoreGive(mutex_);
  if (xTaskCreate(&DebugHttpsTransport::task_entry,"sigap_dbg_http",sigap::network::kTlsWorkerStackBytes,this,1U,nullptr)!=pdPASS) {
    if (xSemaphoreTake(mutex_,portMAX_DELAY)==pdTRUE) { state_=State::Idle; is_quiescent_=true; body_=nullptr; token_=nullptr; body_size_=token_size_=0; xSemaphoreGive(mutex_); }
    return network::TransportResult::Failed;
  }
  return network::TransportResult::Pending;
}
network::TransportResult DebugHttpsTransport::poll(DebugHttpResult& out) noexcept {
  if (!mutex_ || xSemaphoreTake(mutex_,0U)!=pdTRUE) return network::TransportResult::Pending;
  if (!result_ready_) { auto r=state_==State::Running?network::TransportResult::Pending:network::TransportResult::Failed; xSemaphoreGive(mutex_); return r; }
  out=response_; result_ready_=false; const auto r=result_; if (is_quiescent_) state_=State::Idle; xSemaphoreGive(mutex_); return r;
}
void DebugHttpsTransport::cancel() noexcept {
  if (mutex_ && xSemaphoreTake(mutex_,0U)==pdTRUE) { if (state_==State::Running) cancel_requested_=true; xSemaphoreGive(mutex_); }
}
void DebugHttpsTransport::task_entry(void* context) noexcept { static_cast<DebugHttpsTransport*>(context)->run_task(); }
void DebugHttpsTransport::publish(const DebugHttpResult& response, network::TransportResult result) noexcept {
  if (mutex_ && xSemaphoreTake(mutex_,portMAX_DELAY)==pdTRUE) {
    if (!result_published_) { response_=response; result_=cancel_requested_?network::TransportResult::Failed:result; result_ready_=true; result_published_=true; state_=State::Complete; }
    xSemaphoreGive(mutex_);
  }
}
void DebugHttpsTransport::run_task() noexcept {
  DebugHttpResult local{}; network::TransportResult outcome=network::TransportResult::Failed;
  bool canceled=false;
  if (mutex_ && xSemaphoreTake(mutex_,portMAX_DELAY)==pdTRUE) { canceled=cancel_requested_; xSemaphoreGive(mutex_); }
  if (canceled) {
    publish(local,network::TransportResult::Failed);
    if (mutex_ && xSemaphoreTake(mutex_,portMAX_DELAY)==pdTRUE) {
      is_quiescent_=true; state_=State::Complete; body_=nullptr; token_=nullptr; body_size_=token_size_=0U;
      xSemaphoreGive(mutex_);
    }
    vTaskDelete(nullptr);
    return;
  }
  { // Client and HTTP objects are destroyed before quiescence is published.
    WiFiClientSecure client; client.setCACert(ca_);
    HTTPClient http;
    if (http.begin(client,endpoint_.data())) {
      http.setConnectTimeout(static_cast<int32_t>(timeout_ms_));
      http.setTimeout(static_cast<uint16_t>(timeout_ms_));
      std::array<char,136> auth{}; constexpr char prefix[]="Bearer ";
      std::copy(prefix,prefix+7,auth.begin()); std::copy(token_,token_+token_size_,auth.begin()+7);
      http.addHeader("Content-Type","application/json"); http.addHeader("Authorization",auth.data());
      const uint32_t request_started_ms=millis();
      int status=http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(body_)),body_size_);
      local.status_code=status;
      uint32_t elapsed=static_cast<uint32_t>(millis()-request_started_ms);
      if (status>0 && elapsed<timeout_ms_) {
        http.setTimeout(static_cast<uint16_t>(timeout_ms_-elapsed));
        AckCollector collector;
        const int read_result=http.writeToStream(&collector);
        local.ack_body=collector.bytes(); local.ack_size=collector.size();
        const bool read_timed_out=static_cast<uint32_t>(millis()-request_started_ms)>=timeout_ms_;
        local.truncated=collector.truncated() || read_timed_out;
        outcome=(read_timed_out || (read_result<0 && !collector.truncated()))
            ? network::TransportResult::Timeout : network::TransportResult::Response;
      } else outcome=network::TransportResult::Timeout;
      bool was_canceled=false;
      if (mutex_ && xSemaphoreTake(mutex_,portMAX_DELAY)==pdTRUE) { was_canceled=cancel_requested_; xSemaphoreGive(mutex_); }
      publish(local,was_canceled?network::TransportResult::Failed:outcome);
      http.end();
    }
    if (mutex_ && xSemaphoreTake(mutex_,portMAX_DELAY)==pdTRUE) { canceled=cancel_requested_; xSemaphoreGive(mutex_); }
  }
  if (mutex_ && xSemaphoreTake(mutex_,portMAX_DELAY)==pdTRUE) {
    if (!result_published_) { response_=local; result_=canceled?network::TransportResult::Failed:outcome; result_ready_=true; result_published_=true; }
    is_quiescent_=true; state_=result_ready_?State::Complete:State::Idle;
    body_=nullptr; token_=nullptr; body_size_=token_size_=0U;
    xSemaphoreGive(mutex_);
  }
  vTaskDelete(nullptr);
}
}
#endif
