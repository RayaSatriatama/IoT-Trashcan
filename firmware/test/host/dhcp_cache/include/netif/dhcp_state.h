#ifndef SIGAP_HOST_DHCP_STATE_H
#define SIGAP_HOST_DHCP_STATE_H
#ifdef __cplusplus
extern "C" {
#endif
bool dhcp_ip_addr_restore(void *netif);
void dhcp_ip_addr_store(void *netif);
void dhcp_ip_addr_erase(void *esp_netif);
#ifdef __cplusplus
}
#endif
#endif
