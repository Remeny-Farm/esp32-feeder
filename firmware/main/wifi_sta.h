/* Wi-Fi station with auto-reconnect and exponential backoff. */
#ifndef WIFI_STA_H
#define WIFI_STA_H

#include <stdbool.h>

#include "freertos/FreeRTOS.h"

/* Needs nvs_flash_init() and the default event loop. Starts connecting. */
void wifi_sta_start(void);

/* Block until the first IP address, or the timeout. Returns true with an IP. */
bool wifi_sta_wait_ip(TickType_t timeout);

#endif /* WIFI_STA_H */
