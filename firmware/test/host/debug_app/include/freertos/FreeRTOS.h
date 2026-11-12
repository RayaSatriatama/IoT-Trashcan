#pragma once
#include "test_platform.h"
inline thread_local int host_critical_depth=0;
struct portMUX_TYPE {int held=0;};
#define portMUX_INITIALIZER_UNLOCKED {}
inline void portENTER_CRITICAL(portMUX_TYPE*m){assert(m->held++==0);}inline void portEXIT_CRITICAL(portMUX_TYPE*m){assert(--m->held==0);}
using TickType_t=uint32_t;inline constexpr int pdTRUE=1,pdFALSE=0,pdPASS=1;inline constexpr uint32_t portMAX_DELAY=0xffffffff;
#define pdMS_TO_TICKS(x) (x)
