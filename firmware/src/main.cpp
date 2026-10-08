#include "app/sigap_app.h"

#ifdef ARDUINO
#include <cstdlib>
#include <new>
#include <esp_heap_caps.h>
#endif

namespace {
sigap::SigapApp& app() {
#ifdef ARDUINO
  static sigap::SigapApp* instance{nullptr};
  if (instance == nullptr) {
    void* storage = heap_caps_malloc(sizeof(sigap::SigapApp),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storage == nullptr) std::abort();
    instance = new (storage) sigap::SigapApp{};
  }
  return *instance;
#else
  static sigap::SigapApp instance{};
  return instance;
#endif
}
}  // namespace

#ifdef ARDUINO
void setup() { app().setup(); }
void loop() { app().loop(); }
#else
int main() {
  app().setup();
  app().loop();
  return 0;
}
#endif
