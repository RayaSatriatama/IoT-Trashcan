#pragma once
#include "freertos/FreeRTOS.h"
using TaskFunction_t=void(*)(void*);
int xTaskCreate(TaskFunction_t,const char*,uint32_t,void*,uint32_t,void*);
void vTaskDelete(void*);
void taskYIELD();
