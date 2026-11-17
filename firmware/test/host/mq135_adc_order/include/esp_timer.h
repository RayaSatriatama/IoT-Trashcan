#pragma once

// Host stub of the ESP-IDF monotonic timer used by the target adapters.
// The ordering test never advances time; it only needs the symbol to exist.

#include <cstdint>

inline int64_t esp_timer_get_time() { return 0; }
