#include "app/sigap_app.h"

namespace {
sigap::SigapApp& app() {
  static sigap::SigapApp instance{};
  return instance;
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
