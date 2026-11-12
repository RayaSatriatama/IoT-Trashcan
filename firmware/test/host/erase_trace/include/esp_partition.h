#pragma once
#include <cstddef>
#include "esp_err.h"
struct esp_partition_t {};
extern "C" esp_err_t esp_partition_erase_range(const esp_partition_t*, std::size_t, std::size_t);
