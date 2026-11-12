#include <iostream>

#include "debug/registry.h"

int main() {
  using sigap::debug::kEvents;
  using sigap::debug::registered;
  for (const auto& event : kEvents) {
    std::cout << static_cast<unsigned>(event.module) << '\t' << event.code << '\t'
              << (registered(event.module, event.code) ? 1 : 0) << '\t'
              << event.name << '\t' << event.arg0 << '\t' << event.arg1 << '\n';
  }
}
