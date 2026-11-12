#include <cassert>
#include <algorithm>
#include <cstring>
#include <string>
#include "debug/http_transport.h"
#include "HTTPClient.h"
#include "WiFi.h"
#include "WiFiClientSecure.h"
#include "debug/service.cpp"
#include "debug/codec.cpp"
#include "debug/http_worker.cpp"
#include "telemetry/sha256.h"
WiFiClass WiFi;
static FakeSemaphore sem; static TaskFunction_t queued{}; static void* queued_arg{}; static bool fail_task{}; static bool fail_mutex{};
static std::string reply; static int response_status=200, response_size=-2, post_count{}; static uint32_t post_elapsed_ms=7, read_elapsed_ms{}; static std::string sent_body,sent_auth,sent_url,sent_content_type; static uint32_t timeout_seen{},connect_timeout_seen{}; static bool tls_ca{},ended{},tls_destroy_seen{},fail_begin{}; static sigap::debug::DebugHttpsTransport* observing{}; static bool observed_early{}; static sigap::debug::DebugHttpResult early_result{};
static sigap::debug::DebugHttpWorker* integration_worker{}; static sigap::debug::RuntimeIoGate* integration_gate{}; static sigap::debug::DebugReadiness integration_ready{true,true,true,true,false,false}; static bool worker_observed_response{};
static uint32_t clock_ms{}; uint32_t millis(){return clock_ms;}
SemaphoreHandle_t xSemaphoreCreateMutex(){ return fail_mutex?nullptr:&sem; }
int xSemaphoreTake(SemaphoreHandle_t s,TickType_t){ if(s->held)return 0; s->held=true; return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t s){s->held=false;return pdTRUE;}
int xTaskCreate(TaskFunction_t f,const char*,uint32_t,void* a,uint32_t,void*){if(fail_task)return 0;queued=f;queued_arg=a;return pdPASS;}
void vTaskDelete(void*){}
void taskYIELD(){}
WiFiClientSecure::~WiFiClientSecure(){if(integration_worker){assert(!observing->quiescent()); assert(integration_gate->active(sigap::debug::IoOwner::DebugHttp));}else if(observing && observed_early && !tls_destroy_seen){assert(!observing->quiescent()); assert(observing->start("overlap","token",50)==sigap::network::TransportResult::Failed); tls_destroy_seen=true;}}
void WiFiClientSecure::setCACert(const char* ca){tls_ca=ca && *ca;}
class ReplyStream:public Stream{size_t pos{}; public:void reset(){pos=0;} int available()override{return static_cast<int>(reply.size()-pos);} int read()override{return pos<reply.size()?static_cast<unsigned char>(reply[pos++]):-1;} int peek()override{return pos<reply.size()?static_cast<unsigned char>(reply[pos]):-1;} void flush()override{} size_t write(uint8_t)override{return 0;} size_t readBytes(uint8_t* p,size_t n)override{n=std::min(n,reply.size()-pos);std::memcpy(p,reply.data(),n);pos+=n;return n;}} stream;
bool HTTPClient::begin(WiFiClient&,String url){sent_url=url.c_str();return !fail_begin;} void HTTPClient::setTimeout(uint16_t n){timeout_seen=n;} void HTTPClient::setConnectTimeout(int32_t n){connect_timeout_seen=n;} void HTTPClient::addHeader(const String& k,const String& v,bool,bool){if(std::strcmp(k.c_str(),"Authorization")==0)sent_auth=v.c_str();if(std::strcmp(k.c_str(),"Content-Type")==0)sent_content_type=v.c_str();}
int HTTPClient::POST(uint8_t* p,size_t n){++post_count;sent_body.assign(reinterpret_cast<char*>(p),n);clock_ms+=post_elapsed_ms;if(integration_worker){static constexpr char hex[]="0123456789abcdef";auto digest=sigap::telemetry_contract::sha256(sent_body);std::string digest_hex;for(uint8_t b:digest){digest_hex+=hex[b>>4];digest_hex+=hex[b&15];}reply="{\"schema_version\":1,\"status\":\"DURABLE\",\"device_id\":\"device-1\",\"boot_id\":\"00000000000000000000000000000001\",\"first_sequence\":\"1\",\"last_sequence\":\"2\",\"stored_count\":2,\"deduplicated_count\":0,\"request_sha256\":\""+digest_hex+"\"}";}return response_status;} int HTTPClient::getSize(){return response_size==-2?static_cast<int>(reply.size()):response_size;} int HTTPClient::writeToStream(Stream* sink){size_t n=sink->write(reinterpret_cast<const uint8_t*>(reply.data()),reply.size());clock_ms+=read_elapsed_ms;return n==reply.size()?static_cast<int>(n):-10;} void HTTPClient::end(){ended=true; if(integration_worker){integration_worker->poll(5000099U,integration_ready);integration_worker->poll(6000100U,integration_ready);worker_observed_response=true;assert(integration_worker->transport_pending());assert(integration_worker->status()==sigap::debug::DebugWorkerStatus::Waiting);assert(integration_worker->cursor()==0U);assert(integration_gate->active(sigap::debug::IoOwner::DebugHttp));}else if(observing && !observed_early){assert(observing->poll(early_result)==sigap::network::TransportResult::Response); assert(!observing->quiescent()); assert(observing->start("overlap","token",50)==sigap::network::TransportResult::Failed); observed_early=true;}}
int main(){
  using namespace sigap::debug; using sigap::network::TransportResult;
  const std::string body="{\"immutable\":true}",token="secret-test-token"; reply=std::string(512,'A');
  DebugHttpsTransport t("https://example.invalid/path","test-ca");
  assert(t.quiescent()); observing=&t; assert(t.start(body,token,5000)==TransportResult::Pending);
  assert(!t.quiescent()); assert(t.start(body,token,1)==TransportResult::Failed);
  DebugHttpResult result{}; assert(t.poll(result)==TransportResult::Pending);
  assert(queued); queued(queued_arg); assert(observed_early && tls_destroy_seen && t.quiescent());
  assert(early_result.status_code==200 && early_result.ack_size==512 && !early_result.truncated);
  assert(sent_body==body && sent_auth=="Bearer "+token && sent_content_type=="application/json" && sent_url=="https://example.invalid/path" && timeout_seen==4993 && connect_timeout_seen==5000 && post_count==1 && tls_ca && ended);
  reply.push_back('B'); auto repeat_start=t.start(body,token,5000); assert(repeat_start==TransportResult::Pending); queued(queued_arg); assert(t.poll(result)==TransportResult::Response); assert(result.truncated && result.ack_size==512);
  response_size=-1; reply=std::string(257,'C'); assert(t.start(body,token,5000)==TransportResult::Pending); queued(queued_arg); assert(t.poll(result)==TransportResult::Response); assert(!result.truncated && result.ack_size==257 && result.ack_body[256]=='C'); response_size=-2;
  fail_task=true; assert(t.start(body,token,5000)==TransportResult::Failed); assert(t.quiescent());
  fail_task=false; assert(t.start(body,token,65536U)==TransportResult::Failed);
  const int requests_before_cancel=post_count; assert(t.start(body,token,5000)==TransportResult::Pending); t.cancel(); queued(queued_arg);
  assert(t.quiescent()); assert(t.poll(result)==TransportResult::Failed);
  assert(post_count==requests_before_cancel);
  observing=nullptr;
  DebugHttpsTransport mutex_case("https://example.invalid/path","test-ca"); fail_mutex=true; assert(mutex_case.start(body,token,5000)==TransportResult::Failed); assert(mutex_case.quiescent()); fail_mutex=false;
  assert(mutex_case.start(body,token,5000)==TransportResult::Pending); queued(queued_arg); assert(mutex_case.quiescent()); assert(mutex_case.poll(result)==TransportResult::Response);
  DebugHttpsTransport bad_endpoint("http://example.invalid/path","test-ca"); assert(bad_endpoint.start(body,token,5000)==TransportResult::Failed);
  WiFi.connected=false; assert(mutex_case.start(body,token,5000)==TransportResult::Failed); WiFi.connected=true;
  fail_begin=true; assert(mutex_case.start(body,token,5000)==TransportResult::Pending); queued(queued_arg); assert(mutex_case.quiescent()); assert(mutex_case.poll(result)==TransportResult::Failed); fail_begin=false;
  response_status=-11; assert(mutex_case.start(body,token,5000)==TransportResult::Pending); queued(queued_arg); assert(mutex_case.quiescent()); assert(mutex_case.poll(result)==TransportResult::Timeout); response_status=200;
  post_elapsed_ms=5000; assert(mutex_case.start(body,token,5000)==TransportResult::Pending); queued(queued_arg); assert(mutex_case.quiescent()); assert(mutex_case.poll(result)==TransportResult::Timeout); assert(result.status_code==200); post_elapsed_ms=7;
  read_elapsed_ms=5000; assert(mutex_case.start(body,token,5000)==TransportResult::Pending); queued(queued_arg); assert(mutex_case.quiescent()); assert(mutex_case.poll(result)==TransportResult::Timeout); assert(result.status_code==200); read_elapsed_ms=0;
  assert(mutex_case.start(body,token,5000)==TransportResult::Pending); queued(queued_arg); assert(mutex_case.poll(result)==TransportResult::Response);
  struct Lock final : IDebugLock { void lock() noexcept override {} void unlock() noexcept override {} } lock;
  sigap::acquisition::BootId boot{}; boot.present=true; boot.bytes[15]=1U;
  DebugService service(boot,"TEST",Level::Debug,lock); EventInput event{}; event.monotonic_us=10U; event.module=Module::Runtime; event.event_code=1U; assert(service.emit(event)); assert(service.emit(event));
  RuntimeIoGate io_gate; DebugHttpsTransport integrated("https://example.invalid/","test-ca"); DebugHttpWorker worker(service,integrated,io_gate,"device-1");
  const DebugReadiness ready{true,true,true,true,false,false}; assert(worker.configure("https://example.invalid/","worker-token"));
  worker.poll(100U,ready); assert(worker.transport_pending() && io_gate.active(IoOwner::DebugHttp));
  observing=&integrated; integration_worker=&worker; integration_gate=&io_gate; response_size=-2; response_status=200; post_elapsed_ms=7;
  queued(queued_arg); assert(worker_observed_response && integrated.quiescent() && worker.transport_pending());
  assert(worker.cursor()==0U && io_gate.active(IoOwner::DebugHttp));
  worker.poll(6000101U,ready); assert(worker.status()==DebugWorkerStatus::Delivered && worker.cursor()==2U);
  assert(!worker.transport_pending() && !io_gate.active(IoOwner::DebugHttp));
  integration_worker=nullptr; integration_gate=nullptr; observing=nullptr;
  return 0;
}
