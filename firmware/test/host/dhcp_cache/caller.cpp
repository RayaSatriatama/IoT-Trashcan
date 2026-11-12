#include <cassert>
#include <cstdint>
#include <cstring>
#include <netif/dhcp_state.h>

extern "C" unsigned dhcp_probe_restore_calls();
extern "C" unsigned dhcp_probe_store_calls();
extern "C" unsigned dhcp_probe_erase_calls();
extern "C" void dhcp_probe_reset();

int main(int argc, char** argv) {
  const bool positive_control = argc == 2 && std::strcmp(argv[1], "positive") == 0;
  void* opaque[] = {nullptr, reinterpret_cast<void*>(uintptr_t{0x1234}),
                    reinterpret_cast<void*>(uintptr_t{0x5678})};
  dhcp_probe_reset();
  for (unsigned repeat = 0; repeat != 2; ++repeat) {
    for (void* ptr : opaque) {
      const bool restored = dhcp_ip_addr_restore(ptr);
      assert(restored == positive_control);
      dhcp_ip_addr_store(ptr);
      dhcp_ip_addr_erase(ptr);
    }
  }
  if (positive_control) {
    assert(dhcp_probe_restore_calls() == 6);
    assert(dhcp_probe_store_calls() == 6);
    assert(dhcp_probe_erase_calls() == 6);
  } else {
    assert(dhcp_probe_restore_calls() == 0);
    assert(dhcp_probe_store_calls() == 0);
    assert(dhcp_probe_erase_calls() == 0);
  }
}
