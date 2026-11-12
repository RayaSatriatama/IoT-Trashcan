#pragma once
#include "freertos/FreeRTOS.h"
struct FakeSemaphore { bool held{}; };
using SemaphoreHandle_t=FakeSemaphore*;
SemaphoreHandle_t xSemaphoreCreateMutex();
int xSemaphoreTake(SemaphoreHandle_t,TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
