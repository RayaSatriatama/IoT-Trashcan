#include "network/async_ack_source.h"

#ifdef ARDUINO
#include <new>
#include <cstring>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace sigap::network {
struct AsyncAckBatchSource::State final {
  SemaphoreHandle_t mutex{nullptr};
  TaskHandle_t task{nullptr};
  uint32_t main_core{0U};
  int32_t worker_core{-1};
  bool ready{false}, queued{false}, running{false}, done{false};
  telemetry_archive::Ack job_ack{};
  DeliveryStatus result{DeliveryStatus::Retryable};
  uint32_t started{0U}, completed{0U}, stack_hwm{0U};
  uint64_t duration{0U};
};

AsyncAckBatchSource::AsyncAckBatchSource(IImmutableBatchSource& inner) noexcept : inner_(inner) {}
AsyncAckBatchSource::~AsyncAckBatchSource() {
  // The persistent task is app-lifetime; deleting the adapter while active is unsupported.
}
bool AsyncAckBatchSource::begin(uint32_t main_core) noexcept {
  if (state_) {
    if (xSemaphoreTake(state_->mutex, 0U) != pdTRUE) return false;
    const bool accepted = state_->task != nullptr && state_->main_core == main_core;
    xSemaphoreGive(state_->mutex);
    return accepted;
  }
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
  (void)main_core; return false;
#else
  if (main_core >= portNUM_PROCESSORS || portNUM_PROCESSORS < 2) return false;
  State* s = new (std::nothrow) State{};
  if (!s) return false;
  s->main_core = main_core;
  s->mutex = xSemaphoreCreateMutex();
  if (!s->mutex) { delete s; return false; }
  state_ = s;
  const uint32_t worker = main_core == 0U ? 1U : 0U;
  if (xTaskCreatePinnedToCore(task_entry, "sigap_ack", 16U * 1024U, this,
                              tskIDLE_PRIORITY + 1U, &s->task, worker) != pdPASS) {
    state_ = nullptr;
    vSemaphoreDelete(s->mutex);
    delete s;
    return false;
  }
  return true;  // Worker separately validates its actual core before ready().
#endif
}
bool AsyncAckBatchSource::ready() const noexcept {
  State* s=state_; if (!s || xSemaphoreTake(s->mutex,0)!=pdTRUE) return false;
  bool v=s->ready; xSemaphoreGive(s->mutex); return v;
}
bool AsyncAckBatchSource::busy() const noexcept {
  State* s=state_; if (!s) return false;
  if (xSemaphoreTake(s->mutex,0)!=pdTRUE) return true;
  bool v=s->queued || s->running || s->done; xSemaphoreGive(s->mutex); return v;
}
bool AsyncAckBatchSource::peek_pending(const telemetry_archive::ImmutableBatch*& batch) noexcept {
  batch=nullptr; if (busy()) return false; return inner_.peek_pending(batch);
}
DeliveryStatus AsyncAckBatchSource::acknowledge(const telemetry_archive::Ack& ack) noexcept {
  State* s=state_; if (!s) return DeliveryStatus::Retryable;
  if (xSemaphoreTake(s->mutex,0)!=pdTRUE) return DeliveryStatus::Pending;
  if (!s->ready) { xSemaphoreGive(s->mutex); return DeliveryStatus::Retryable; }
  auto same=[](const telemetry_archive::Ack&a,const telemetry_archive::Ack&b) noexcept {
    return a.batch_id_size==b.batch_id_size && a.batch_id_size<=a.batch_id.size() &&
      std::equal(a.batch_id.begin(),a.batch_id.begin()+a.batch_id_size,b.batch_id.begin()) &&
      a.body_hash==b.body_hash && a.record_count==b.record_count && a.stored_count==b.stored_count &&
      a.deduplicated_count==b.deduplicated_count && a.device_id.present==b.device_id.present &&
      a.device_id.bytes==b.device_id.bytes && a.status==b.status;
  };
  if (s->done) {
    if (!same(ack,s->job_ack)) { xSemaphoreGive(s->mutex); return DeliveryStatus::Pending; }
    auto result=s->result; s->done=false; xSemaphoreGive(s->mutex); return result;
  }
  if (s->queued || s->running) { xSemaphoreGive(s->mutex); return DeliveryStatus::Pending; }
  s->job_ack=ack; s->queued=true; ++s->started; TaskHandle_t task=s->task;
  xSemaphoreGive(s->mutex); if (task) xTaskNotifyGive(task); return DeliveryStatus::Pending;
}
AckWorkerSnapshot AsyncAckBatchSource::snapshot() const noexcept {
  AckWorkerSnapshot out{}; State*s=state_;
  if (!s) { out.valid=true; out.busy=false; return out; }
  if(xSemaphoreTake(s->mutex,0)!=pdTRUE) return out;
  out.valid=true; out.ready=s->ready; out.busy=s->queued||s->running||s->done; out.main_core=s->main_core;
  out.worker_core=s->worker_core; out.started=s->started; out.completed=s->completed; out.stack_hwm=s->stack_hwm;
  out.last_duration_us=s->duration; out.last_status=s->result; xSemaphoreGive(s->mutex); return out;
}
void AsyncAckBatchSource::task_entry(void* context) noexcept {
  auto*self=static_cast<AsyncAckBatchSource*>(context); State*s=self->state_;
  const int32_t actual_core=static_cast<int32_t>(xPortGetCoreID());
  if (actual_core<0 || static_cast<uint32_t>(actual_core)==s->main_core ||
      static_cast<uint32_t>(actual_core)>=portNUM_PROCESSORS) {
    if (xSemaphoreTake(s->mutex,portMAX_DELAY)==pdTRUE) {
      s->worker_core=actual_core;
      s->task=nullptr;
      s->ready=false;
      xSemaphoreGive(s->mutex);
    }
    vTaskDelete(nullptr);
    return;
  }
  if (xSemaphoreTake(s->mutex,portMAX_DELAY)==pdTRUE) {
    s->worker_core=actual_core; s->ready=true; xSemaphoreGive(s->mutex);
  }
  for (;;) {
    ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    if (xSemaphoreTake(s->mutex,portMAX_DELAY)!=pdTRUE) continue;
    if (!s->queued) { xSemaphoreGive(s->mutex); continue; }
    auto ack=s->job_ack; s->queued=false; s->running=true; xSemaphoreGive(s->mutex);
    const int64_t started=esp_timer_get_time();
    const auto result=self->inner_.acknowledge(ack);
    const uint64_t elapsed=static_cast<uint64_t>(esp_timer_get_time()-started);
    const int32_t core=static_cast<int32_t>(xPortGetCoreID());
    const uint32_t hwm=uxTaskGetStackHighWaterMark(nullptr);
    if (xSemaphoreTake(s->mutex,portMAX_DELAY)==pdTRUE) {
      s->running=false; s->done=true; s->result=result; ++s->completed; s->duration=elapsed;
      s->worker_core=core; s->stack_hwm=hwm; xSemaphoreGive(s->mutex);
    }
  }
}
}  // namespace sigap::network
#endif
