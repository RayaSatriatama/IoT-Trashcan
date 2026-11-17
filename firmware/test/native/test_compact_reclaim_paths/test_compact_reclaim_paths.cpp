#define SIGAP_FRESH_WEB_PRIORITY14 1
#include <unity.h>
#include <cstring>
#include <string>
#include "storage/live_delivery_paths.h"

using sigap::acquisition::BootId;
using sigap::storage::live_delivery::BootPaths;
using sigap::storage::live_delivery::related_suffix;

namespace {
BootId boot_id() {
  BootId id{};
  id.present = true;
  id.bytes[0] = 1U;
  return id;
}

void test_canonical_archive_compact_backup_suffix() {
  const BootPaths paths(boot_id());
  TEST_ASSERT_TRUE(paths.valid);
  TEST_ASSERT_EQUAL_STRING(".mb", related_suffix(paths.archive.data(), ".migrate.bak"));
  const std::string backup = std::string(paths.archive.data()) +
      related_suffix(paths.archive.data(), ".migrate.bak");
  const char* basename = std::strrchr(backup.c_str(), '/');
  TEST_ASSERT_NOT_NULL(basename);
  TEST_ASSERT_LESS_OR_EQUAL_UINT(32U, std::strlen(basename + 1));
}

void test_other_archive_suffixes_and_paths_are_unchanged() {
  const BootPaths paths(boot_id());
  const char* unchanged[]{".recover", ".migrate", ".repair", ".tail0"};
  for (const char* suffix : unchanged) {
    TEST_ASSERT_EQUAL_STRING(suffix, related_suffix(paths.archive.data(), suffix));
  }
  TEST_ASSERT_EQUAL_STRING(".migrate.bak", related_suffix("/sigap/archive.bin", ".migrate.bak"));
  TEST_ASSERT_EQUAL_STRING(".migrate.bak", related_suffix("/sigap/web-sync-v3.bin", ".migrate.bak"));
  TEST_ASSERT_EQUAL_STRING(".migrate.bak", related_suffix(paths.batches.data(), ".migrate.bak"));
  TEST_ASSERT_EQUAL_STRING(".b", related_suffix(paths.batches.data(), ".b"));
  TEST_ASSERT_EQUAL_STRING(".recover", related_suffix(paths.batches.data(), ".recover"));
  TEST_ASSERT_EQUAL_STRING(".migrate.bak", related_suffix(nullptr, ".migrate.bak"));
  TEST_ASSERT_NULL(related_suffix(paths.archive.data(), nullptr));
}
}

void setUp() {}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_canonical_archive_compact_backup_suffix);
  RUN_TEST(test_other_archive_suffixes_and_paths_are_unchanged);
  return UNITY_END();
}
