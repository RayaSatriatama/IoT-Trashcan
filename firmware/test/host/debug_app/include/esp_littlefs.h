#pragma once
#include "nvs.h"
#include <cstddef>
#include <cstring>

namespace test_littlefs {
struct InfoFixture {
  esp_err_t status{ESP_OK};
  std::size_t total{8U * 1024U * 1024U};
  std::size_t used{0U};
  unsigned calls{0U};
  char observed_label[16]{};
};
inline InfoFixture fixture{};
inline void reset() noexcept { fixture = InfoFixture{}; }
}
inline esp_err_t esp_littlefs_info(const char* label, std::size_t* total, std::size_t* used) {
  ++test_littlefs::fixture.calls;
  if (label == nullptr) test_littlefs::fixture.observed_label[0] = '\0';
  else {
    std::strncpy(test_littlefs::fixture.observed_label, label,
                 sizeof(test_littlefs::fixture.observed_label) - 1U);
    test_littlefs::fixture.observed_label[sizeof(test_littlefs::fixture.observed_label) - 1U] = '\0';
  }
  if (test_littlefs::fixture.status != ESP_OK) return test_littlefs::fixture.status;
  if (total != nullptr) *total = test_littlefs::fixture.total;
  if (used != nullptr) *used = test_littlefs::fixture.used;
  return ESP_OK;
}
