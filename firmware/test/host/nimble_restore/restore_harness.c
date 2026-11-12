/* Host-only NVS restore reproducer. No ESP32 driver or real NVS is linked. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NIMBLE_STORE_SOURCE
#error Define NIMBLE_STORE_SOURCE as the inspected production C file path.
#endif
#include NIMBLE_STORE_SOURCE

/* Real store record types and declarations come from the pinned headers. */
struct ble_store_value_sec ble_store_config_our_secs[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
struct ble_store_value_sec ble_store_config_peer_secs[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
struct ble_store_value_cccd ble_store_config_cccds[MYNEWT_VAL(BLE_STORE_MAX_CCCDS)];
struct ble_store_value_csfc ble_store_config_csfcs[MYNEWT_VAL(BLE_STORE_MAX_CSFCS)];
struct ble_store_value_rpa_rec ble_store_config_rpa_recs[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
struct ble_store_value_local_irk ble_store_config_local_irks[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
int ble_store_config_num_our_secs;
int ble_store_config_num_peer_secs;
int ble_store_config_num_cccds;
int ble_store_config_num_csfcs;
int ble_store_config_num_rpa_recs;
int ble_store_config_num_local_irks;
uint16_t ble_store_config_our_bond_count;
uint16_t ble_store_config_peer_bond_count;

static const char *fixture;
static unsigned write_calls;
static unsigned open_calls;

int ble_store_config_compare_bond_count(const void *left, const void *right) {
    const struct ble_store_value_sec *a = left;
    const struct ble_store_value_sec *b = right;
    return (a->bond_count > b->bond_count) - (a->bond_count < b->bond_count);
}

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    assert(strcmp(name, "nimble_bond") == 0);
    assert(mode == NVS_READWRITE);
    ++open_calls;
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key,
                       void *output, size_t *size) {
    assert(handle == 1);
    if (strcmp(fixture, "read-error") == 0) return ESP_FAIL;
    const int own_present = strcmp(fixture, "both") == 0 ||
                            strcmp(fixture, "our-only") == 0;
    const int peer_present = strcmp(fixture, "both") == 0 ||
                             strcmp(fixture, "peer-only") == 0;
    const int own_key = strcmp(key, "our_sec_1") == 0;
    const int peer_key = strcmp(key, "peer_sec_1") == 0;
    if (!((own_key && own_present) || (peer_key && peer_present))) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    struct ble_store_value_sec value = {0};
    /* Synthetic fixture metadata only. No actual address or key bytes. */
    value.bond_count = own_key ? 7 : 11;
    if (output != NULL) {
        assert(*size >= sizeof(value));
        memcpy(output, &value, sizeof(value));
    }
    *size = sizeof(value);
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { assert(handle == 1); }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t n) {
    (void)h; (void)k; (void)v; (void)n; ++write_calls; return ESP_FAIL;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k) {
    (void)h; (void)k; ++write_calls; return ESP_FAIL;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h; ++write_calls; return ESP_FAIL;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    fixture = argv[1];
    const int valid = strcmp(fixture, "empty") == 0 ||
                      strcmp(fixture, "both") == 0 ||
                      strcmp(fixture, "our-only") == 0 ||
                      strcmp(fixture, "peer-only") == 0 ||
                      strcmp(fixture, "read-error") == 0;
    if (!valid) return 2;
    const int rc = ble_nvs_restore_sec_keys();
    assert(open_calls > 0);
    assert(write_calls == 0);
    if (strcmp(fixture, "read-error") == 0) {
        assert(rc != 0);
        assert(ble_store_config_num_our_secs == 0);
        assert(ble_store_config_num_peer_secs == 0);
    } else {
        const int own_present = strcmp(fixture, "both") == 0 ||
                                strcmp(fixture, "our-only") == 0;
        const int peer_present = strcmp(fixture, "both") == 0 ||
                                 strcmp(fixture, "peer-only") == 0;
        assert(rc == 0);
        assert(ble_store_config_num_our_secs == own_present);
        assert(ble_store_config_num_peer_secs == peer_present);
        assert(ble_store_config_our_bond_count == (own_present ? 7 : 0));
        assert(ble_store_config_peer_bond_count == (peer_present ? 11 : 0));
    }
    printf("PASS fixture=%s writes=%u\n", fixture, write_calls);
    return 0;
}
