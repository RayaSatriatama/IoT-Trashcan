#pragma once
#include "esp_partition.h"
using nvs_handle_t = unsigned;
extern "C" esp_err_t nvs_flash_erase(void);
extern "C" esp_err_t nvs_flash_erase_partition(const char*);
extern "C" esp_err_t nvs_flash_erase_partition_ptr(const esp_partition_t*);
extern "C" esp_err_t nvs_erase_key(nvs_handle_t, const char*);
extern "C" esp_err_t nvs_erase_all(nvs_handle_t);
