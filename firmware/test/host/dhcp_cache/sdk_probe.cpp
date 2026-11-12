#include <atomic>
#include <netif/dhcp_state.h>

namespace {
std::atomic<unsigned> restore_calls{0};
std::atomic<unsigned> store_calls{0};
std::atomic<unsigned> erase_calls{0};
}

extern "C" bool dhcp_ip_addr_restore(void*) { ++restore_calls; return true; }
extern "C" void dhcp_ip_addr_store(void*) { ++store_calls; }
extern "C" void dhcp_ip_addr_erase(void*) { ++erase_calls; }
extern "C" unsigned dhcp_probe_restore_calls() { return restore_calls.load(); }
extern "C" unsigned dhcp_probe_store_calls() { return store_calls.load(); }
extern "C" unsigned dhcp_probe_erase_calls() { return erase_calls.load(); }
extern "C" void dhcp_probe_reset() {
  restore_calls.store(0);
  store_calls.store(0);
  erase_calls.store(0);
}
