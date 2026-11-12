#pragma once
#include <cstdint>
enum {ESP_RST_POWERON,ESP_RST_BROWNOUT,ESP_RST_TASK_WDT,ESP_RST_WDT,ESP_RST_INT_WDT,ESP_RST_SW};
inline int esp_reset_reason(){return ESP_RST_POWERON;}
inline uint32_t esp_random(){static uint32_t x=1;return ++x;}
