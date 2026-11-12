#pragma once
#include <stdint.h>
/* Exact address layout from pinned ble.h:295-298; no radio/OS dependency. */
typedef struct { uint8_t type; uint8_t val[6]; } ble_addr_t;
