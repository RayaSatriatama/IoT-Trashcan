#pragma once
#include "FreeRTOS.h"
struct FakeSemaphore {bool available=true;};using SemaphoreHandle_t=FakeSemaphore*;
inline SemaphoreHandle_t xSemaphoreCreateMutex(){return new FakeSemaphore;}
inline SemaphoreHandle_t xSemaphoreCreateBinary(){return new FakeSemaphore{false};}
inline int xSemaphoreTake(SemaphoreHandle_t s,TickType_t){if(!s||!s->available)return 0;s->available=false;return 1;}
inline int xSemaphoreGive(SemaphoreHandle_t s){s->available=true;return 1;}
inline void vSemaphoreDelete(SemaphoreHandle_t s){delete s;}
