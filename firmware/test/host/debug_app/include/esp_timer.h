#pragma once
#include "test_platform.h"
inline int64_t esp_timer_get_time(){return test_platform::now_us;}
