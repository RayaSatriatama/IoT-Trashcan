// Diagnostic-only policy: skip optional SDK remembered-IP cache access.
#include <netif/dhcp_state.h>

#ifndef SIGAP_BLE_WIFI_TEST
#error "DHCP cache suppression is restricted to the diagnostic Wi-Fi test target"
#endif

extern "C" bool __wrap_dhcp_ip_addr_restore(void*) { return false; }
extern "C" void __wrap_dhcp_ip_addr_store(void*) {}
extern "C" void __wrap_dhcp_ip_addr_erase(void*) {}
