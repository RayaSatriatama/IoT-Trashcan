#pragma once
#include "FreeRTOS.h"
using TaskFunction_t=void(*)(void*);
inline int xTaskCreate(TaskFunction_t f,const char*name,uint32_t,void*a,uint32_t,void*){test_platform::tasks.push_back({f,a});std::string n(name);if(n=="sigap_dbg_http")++test_platform::debug_starts;else if(n=="sigap_ctl")++test_platform::control_starts;else {assert(n=="sigap_net");++test_platform::archive_starts;}return pdPASS;}
inline void vTaskDelete(void*){}inline void taskYIELD(){}
inline void vTaskDelay(uint32_t ms){test_platform::now_us+=static_cast<uint64_t>(ms)*1000;}
