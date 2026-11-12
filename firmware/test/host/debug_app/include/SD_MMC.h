#pragma once
#include "FS.h"
inline constexpr unsigned SDMMC_FREQ_DEFAULT=20000;
struct SDMMCClass : fs::FS {
  bool setPins(int,int,int) { return true; }
  bool begin(const char*,bool,bool,unsigned,unsigned) {
    ++test_platform::sd_begins;
    return true;
  }
  uint64_t totalBytes() { return 1024ULL*1024ULL*1024ULL; }
  uint64_t usedBytes() { return 0; }
};
inline SDMMCClass SD_MMC;
