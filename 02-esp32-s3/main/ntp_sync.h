#ifndef NTP_SYNC_H
#define NTP_SYNC_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t ntp_sync_start(void);
bool ntp_sync_wait(uint32_t timeout_ms);
bool ntp_sync_is_synced(void);
void ntp_sync_print(void);

#endif